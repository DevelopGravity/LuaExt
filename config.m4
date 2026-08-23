dnl luaext — a sandbox for running untrusted Lua 5.5 inside PHP.
dnl
dnl The vendored interpreter and the extension's own sources are compiled with
dnl deliberately different flag sets, so they are added to the build separately:
dnl
dnl   third_party/lua-5.5.1/src   upstream code, warnings suppressed, every
dnl                               symbol hidden so an already-loaded liblua can
dnl                               never collide with ours
dnl   src                         our code, compiled with warnings on
dnl
dnl Both share -DLUAEXT_LUA_HOOKS=1: it activates the sandbox overlay at the end
dnl of the vendored luaconf.h, and the declarations our sources see must match
dnl the ones the interpreter was built with.

PHP_ARG_ENABLE([luaext],
  [whether to enable luaext],
  [AS_HELP_STRING([--enable-luaext],
    [Enable luaext, a Lua 5.5 sandbox for untrusted code])])

PHP_ARG_ENABLE([luaext-debug],
  [whether to build luaext with debug assertions],
  [AS_HELP_STRING([--enable-luaext-debug],
    [LUAEXT: enable internal assertions and the Lua API checker])],
  [no],
  [no])

PHP_ARG_ENABLE([luaext-sanitize],
  [which sanitizers to build luaext with],
  [AS_HELP_STRING([--enable-luaext-sanitize@<:@=LIST@:>@],
    [LUAEXT: comma-separated -fsanitize list, e.g. address,undefined])],
  [no],
  [no])

if test "$PHP_LUAEXT" != "no"; then
  AC_DEFINE([HAVE_LUAEXT], [1], [Define to 1 if the luaext extension is built.])

  dnl --------------------------------------------------------------------
  dnl Vendored interpreter
  dnl
  dnl third_party/lua-5.5.1/SOURCES is the authoritative list of translation
  dnl units; it is parsed rather than duplicated so that adding or dropping one
  dnl is a single-file change. Blank lines and comments are ignored.
  dnl --------------------------------------------------------------------
  LUAEXT_LUA_RELDIR="third_party/lua-5.5.1/src"
  LUAEXT_LUA_MANIFEST="$abs_srcdir/third_party/lua-5.5.1/SOURCES"

  AC_MSG_CHECKING([for the vendored Lua source manifest])
  if test ! -f "$LUAEXT_LUA_MANIFEST"; then
    AC_MSG_RESULT([not found])
    AC_MSG_ERROR([missing $LUAEXT_LUA_MANIFEST; the vendored Lua tree is incomplete])
  fi
  AC_MSG_RESULT([$LUAEXT_LUA_MANIFEST])

  LUAEXT_LUA_SOURCES=$($AWK '{
      sub(/#.*$/, "");
      gsub(/[[ \t\r]]/, "");
      if ($[]0 != "") printf "%s ", $[]0;
    }' "$LUAEXT_LUA_MANIFEST")

  AC_MSG_CHECKING([for vendored Lua translation units])
  LUAEXT_LUA_COUNT=$(echo "$LUAEXT_LUA_SOURCES" | wc -w | tr -d ' ')
  if test "$LUAEXT_LUA_COUNT" -lt 1; then
    AC_MSG_RESULT([none])
    AC_MSG_ERROR([$LUAEXT_LUA_MANIFEST lists no source files])
  fi
  AC_MSG_RESULT([$LUAEXT_LUA_COUNT])

  dnl --------------------------------------------------------------------
  dnl Threads
  dnl
  dnl The CPU and wall-clock watchdog owns one OS thread, so pthreads is a hard
  dnl requirement rather than an optional feature.
  dnl --------------------------------------------------------------------
  AX_CHECK_COMPILE_FLAG([-pthread],
    [LUAEXT_PTHREAD_FLAG="-pthread"],
    [LUAEXT_PTHREAD_FLAG=""])

  LUAEXT_SAVED_CFLAGS=$CFLAGS
  LUAEXT_SAVED_LIBS=$LIBS
  CFLAGS="$CFLAGS $LUAEXT_PTHREAD_FLAG"
  LIBS="$LIBS $LUAEXT_PTHREAD_FLAG"

  AC_CHECK_HEADER([pthread.h],,
    [AC_MSG_ERROR([luaext requires POSIX threads: pthread.h not found])])

  AC_CACHE_CHECK([whether pthread_create can be linked],
    [luaext_cv_pthread_create], [
      AC_LINK_IFELSE([AC_LANG_PROGRAM(
        [[#include <pthread.h>
          static void *luaext_probe(void *arg) { return arg; }]],
        [[pthread_t thread;
          return pthread_create(&thread, NULL, luaext_probe, NULL);]])],
        [luaext_cv_pthread_create=yes],
        [luaext_cv_pthread_create=no])])

  CFLAGS=$LUAEXT_SAVED_CFLAGS
  LIBS=$LUAEXT_SAVED_LIBS

  AS_VAR_IF([luaext_cv_pthread_create], [yes],,
    [AC_MSG_ERROR([luaext requires POSIX threads: cannot link pthread_create])])

  dnl The interrupt flag the watchdog raises is a C11 atomic; the vendored
  dnl interrupt-check header includes <stdatomic.h> unconditionally.
  AC_CHECK_HEADER([stdatomic.h],,
    [AC_MSG_ERROR([luaext requires a C11 compiler with <stdatomic.h>])])

  dnl --------------------------------------------------------------------
  dnl Flag sets
  dnl --------------------------------------------------------------------
  LUAEXT_COMMON_FLAGS="-DLUAEXT_LUA_HOOKS=1 $LUAEXT_PTHREAD_FLAG"

  if test "$PHP_LUAEXT_DEBUG" != "no"; then
    AC_MSG_NOTICE([luaext: debug assertions and the Lua API checker are enabled])
    LUAEXT_COMMON_FLAGS="$LUAEXT_COMMON_FLAGS -DLUAEXT_DEBUG=1 -DLUA_USE_APICHECK=1 -g"
  fi

  if test "$PHP_LUAEXT_SANITIZE" != "no"; then
    dnl --enable-luaext-sanitize with no list means "the usual two".
    AS_VAR_IF([PHP_LUAEXT_SANITIZE], [yes],
      [LUAEXT_SANITIZERS="address,undefined"],
      [LUAEXT_SANITIZERS=$PHP_LUAEXT_SANITIZE])

    AX_CHECK_COMPILE_FLAG([-fsanitize=$LUAEXT_SANITIZERS],,
      [AC_MSG_ERROR([the compiler rejects -fsanitize=$LUAEXT_SANITIZERS])])

    AC_MSG_NOTICE([luaext: building with -fsanitize=$LUAEXT_SANITIZERS])
    LUAEXT_COMMON_FLAGS="$LUAEXT_COMMON_FLAGS -fsanitize=$LUAEXT_SANITIZERS -fno-omit-frame-pointer -g"
    LDFLAGS="$LDFLAGS -fsanitize=$LUAEXT_SANITIZERS"
  fi

  dnl Upstream Lua is warning-clean only under its own build flags, and its
  dnl diagnostics are not ours to fix; -w keeps them out of our build log.
  dnl -fvisibility=hidden is load bearing: upstream declares lua_ident with no
  dnl API macro, so the luaconf.h overlay alone cannot hide every symbol, and a
  dnl process that also loads a real liblua must not see two of them.
  LUAEXT_LUA_FLAGS="$LUAEXT_COMMON_FLAGS -w"

  dnl -Wno-unused-parameter for the same reason php-src itself sets it: every
  dnl internal method is handed both execute_data and return_value whether or
  dnl not it has a use for them.
  LUAEXT_FLAGS="$LUAEXT_COMMON_FLAGS -Wall -Wextra -Wno-unused-parameter"

  dnl Applied to our sources too, not just the vendored ones: get_module() is
  dnl marked ZEND_DLEXPORT (explicit default visibility), so hiding everything
  dnl else leaves the module exporting exactly the one symbol PHP looks up.
  AX_CHECK_COMPILE_FLAG([-fvisibility=hidden], [
    LUAEXT_LUA_FLAGS="$LUAEXT_LUA_FLAGS -fvisibility=hidden"
    LUAEXT_FLAGS="$LUAEXT_FLAGS -fvisibility=hidden"
  ], [AC_MSG_WARN([the compiler does not support -fvisibility=hidden; luaext will export Lua symbols])])

  PHP_ADD_INCLUDE([$abs_srcdir/src])
  PHP_ADD_INCLUDE([$abs_srcdir/$LUAEXT_LUA_RELDIR])

  PHP_NEW_EXTENSION([luaext],
    [src/luaext.c \
     src/luaext_sandbox.c \
     src/luaext_interrupt.c \
     src/luaext_pending.c],
    [$ext_shared],,
    [$LUAEXT_FLAGS])

  dnl PHP_NEW_EXTENSION only knows one flag set, so the interpreter is added
  dnl afterwards, into the same object list the module is linked from:
  dnl shared_objects_luaext for a phpize/PIE build, PHP_GLOBAL_OBJS when the
  dnl extension is compiled into php itself.
  if test -z "$ext_dir"; then
    LUAEXT_LUA_DIR="$LUAEXT_LUA_RELDIR"
  else
    LUAEXT_LUA_DIR="$ext_dir/$LUAEXT_LUA_RELDIR"
  fi

  PHP_ADD_BUILD_DIR([$ext_builddir/src])
  PHP_ADD_BUILD_DIR([$ext_builddir/$LUAEXT_LUA_RELDIR])

  if test "$ext_shared" = "yes"; then
    PHP_ADD_SOURCES_X([$LUAEXT_LUA_DIR], [$LUAEXT_LUA_SOURCES],
      [$LUAEXT_LUA_FLAGS], [shared_objects_luaext], [yes])
  else
    PHP_ADD_SOURCES([$LUAEXT_LUA_DIR], [$LUAEXT_LUA_SOURCES],
      [$LUAEXT_LUA_FLAGS])
  fi

  PHP_EVAL_LIBLINE([$LUAEXT_PTHREAD_FLAG], [LUAEXT_SHARED_LIBADD])
  PHP_SUBST([LUAEXT_SHARED_LIBADD])
fi
