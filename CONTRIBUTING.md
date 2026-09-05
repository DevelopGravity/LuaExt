# Contributing to LuaExt

This extension runs **untrusted code**. That single fact is behind most of the rules
below, and it is why some of them are stricter than they would be elsewhere: a mistake
here is not a wrong answer, it is a script getting something it was not granted.

Everything here is enforced by something. Where it isn't, it says so.

## Building

```bash
make            # phpize && ./configure && make — a release build
make test       # the above, then the .phpt suite (137 tests)
make dev        # build + test + check — run this before you push
```

`make` at the repo root reads **`GNUmakefile`**, which includes `Makefile.dev`. GNU make
prefers `GNUmakefile` over the `Makefile` that `phpize` generates, and that is deliberate —
see [§ `make clean` reaches further than you think](#make-clean-reaches-further-than-you-think).
To reach the generated one directly: `make -f Makefile <target>`.

No system Lua is required, ever. Lua 5.5.1 is vendored under `third_party/` and compiled
in-tree, patched (`tools/vendor-lua.sh --check` verifies the tree still matches the pinned
tarball plus `patches/*.patch`).

### The debug build

```bash
make build-debug    # ./configure --enable-luaext-debug
```

`--enable-luaext-debug` turns on `LUAEXT_ASSERT` and Lua's own API checker. **Use
`LUAEXT_ASSERT`, never `ZEND_ASSERT`**: php-src defines `ZEND_ASSERT` as `ZEND_ASSUME`
whenever `ZEND_DEBUG` is off, which is a promise to the optimiser rather than a check —
so a `ZEND_ASSERT` in a normal build is not verified, and a wrong one is undefined
behaviour instead of a caught bug.

### The debug PHP, and why you need one

**PHP's leak tracker exists only in a PHP built with `--enable-debug`.** A release build
reports nothing, and macOS `leaks` reports nothing either — it sees the request arena
freed wholesale at shutdown and calls it clean. Four leaks lived in this extension for
several waves because of exactly that.

There is no committed script for this (the local driver is gitignored), so here is the
recipe:

```bash
# Anywhere outside the checkout is fine; .tools/ is gitignored and convenient.
DEBUG_PREFIX="$PWD/.tools/php-debug"

# 1. A minimal debug PHP, matched to the version you normally build against.
git clone --depth 1 --branch php-8.5.10 https://github.com/php/php-src .tools/php-src
( cd .tools/php-src \
  && ./buildconf --force \
  && ./configure --prefix="$DEBUG_PREFIX" --disable-all --enable-cli --enable-debug --without-pear \
  && make -j8 && make install )

# 2. Point the extension at it instead of your usual PHP. Note phpize comes from
#    the debug prefix too -- the build must be configured by the same PHP that
#    will load the result.
phpize --clean
"$DEBUG_PREFIX/bin/phpize"
./configure --with-php-config="$DEBUG_PREFIX/bin/php-config" --enable-luaext --enable-luaext-debug
make -f Makefile -j8 && NO_INTERACTION=1 make -f Makefile test
```

A leaking test then prints, into its own `.diff`:

```
zend_string.h(167) : Freeing 0x... (32 bytes), script=...
=== Total 3 memory leaks detected ===
```

`--disable-all --enable-cli` is the same recipe `ci.yml`'s sanitizer legs use. On macOS,
`buildconf` needs bison 3.0+; Homebrew's is keg-only, so prepend
`/opt/homebrew/opt/bison/bin` and `/opt/homebrew/opt/re2c/bin` to `PATH`.

**Switch back afterwards.** `phpize`/`configure` write one Makefile for one interpreter,
so a tree left configured against the debug PHP stays that way and every later `make test`
runs the slow build.

If you keep a driver script at `.tools/test-debug-php.sh`, `make leak-check` will use it.

### `make clean` reaches further than you think

The **generated** `Makefile`'s `clean` target is copied verbatim out of php-src's
`Makefile.global` and runs, from the repository root:

```sh
find . -name \*.lo -o -name \*.o -o -name \*.dep | xargs rm -f
find . -name .libs -a -type d | xargs rm -rf
```

`find .` has never heard of `.gitignore`. It walks into `.tools/`, `.venv/`, a worktree —
anything parked in the tree. It deleted a 300 MB php-src build here once.

`Makefile.global` belongs to your PHP installation, offers no `clean-local` hook, and any
edit to the generated `Makefile` dies at the next `./configure` — so the fix is the
shipped `GNUmakefile`, which shadows the target with a **scoped** `clean` that prunes
`.git`, `.tools`, `.venv` and `.idea`.

- `make clean` — safe. Cleans this project's build output only.
- `make -f Makefile clean` — the original, unscoped. Still there on purpose.

Disk: a checkout plus a release build is ~80 MB. A local debug PHP under `.tools/` adds
roughly 350 MB more.

## Before you push

```bash
make dev
```

`make check` on its own runs every gate CI runs, and nothing else:

| Gate | Catches |
|---|---|
| `clang-format` | formatting drift. **Pinned to 18.1.8** — versions disagree, and a mismatched local binary produces a diff CI rejects. `.venv/bin/clang-format` is picked up automatically. |
| `editorconfig` | charset, line endings, final newline, trailing whitespace. Indentation is deliberately *not* checked — clang-format owns the C sources. |
| `shellcheck` | `tools/*.sh` |
| `actionlint` | workflow syntax |
| `workflow-php-version` | every workflow pinning the same PHP version |
| `generated-artifacts` | the vendored Lua tree, `*_arginfo.h`, the build-file source lists, and the stdlib golden files all matching what generates them |
| `docs-api` | documentation naming API that does not exist — including wrong named arguments, which a syntax check cannot see |
| `watchdog-purity` | the watchdog reaching anything PHP or Lua owns |
| `banned-idioms` | C that compiles cleanly and fails somewhere expensive |
| `linkage` | the built module's symbol surface |

**A skipped gate is not a passing gate.** Four need a binary that may not be installed;
each reports `SKIPPED` with an install hint and **exits non-zero**. `make check
ALLOW_SKIP=1` downgrades that to a warning — which makes the loose behaviour something you
ask for by name.

**If you add a gate to CI, add it to `Makefile.dev`'s `check` in the same change.** The
list is a local copy of what `lint.yml` and `build-matrix.yml` run, and a `make check`
missing one lies by omission. `tools/check-lua-upstream.sh` is deliberately absent: it is a
scheduled network poll, not a pre-push gate.

## Tests

`.phpt`, run by `run-tests.php`. Where a new one goes:

| Directory | For |
|---|---|
| `00-build` | the extension loading, class registration, `features()` |
| `01-basic` | the ordinary API surface |
| `02-limits` | CPU, wall-clock, memory, output budgets — **the timing-sensitive ones** |
| `03-adversarial` | a script trying to escape something. **Append-only** |
| `04-profiler` | sampling |
| `05-coroutines` | coroutine lifecycle and scoping |
| `06-vfs` | the virtual filesystem and `FileSystem` backends |
| `07-output` | the output sink and its modes |
| `08-require` | module resolution |
| `09-conversion` | the PHP↔Lua value boundary |
| `10-lua` | **Lua language conformance** — that the patched interpreter still computes Lua's answers |

Three rules that are not obvious:

- **`03-adversarial/` is append-only.** Every security finding lands there as a permanent
  test reproducing the attack *before* its fix merges. A fix without one is not finished.
- **`10-lua/` must stay platform-neutral.** Windows runs this directory in CI, so no CPU
  or wall-clock assertions (Windows' thread clock is a ~15.6 ms scheduler tick), no host
  paths, no locale-dependent formatting, and no assumptions about table iteration order —
  hash order is seeded per sandbox from a CSPRNG on purpose.
- **Timing assertions belong in `02-limits/` only**, and they must survive a contended
  shared runner. Assert *that* a budget ran out, never how long it took.

`make test TEST_TIMEOUT=60` raises the per-test timeout for a slow machine or a sanitized
build. The default is 10s because `03-adversarial/` is full of scripts that loop forever
on purpose.

## C conventions

### The rule that has cost the most bugs

**`luaext_error_raise()` ends in `lua_error()`, which longjmps. A longjmp runs no C
cleanup at all.** So a frame holding a `zend_string`, an owned `zval`, or an `emalloc`'d
buffer across *anything that can raise* leaks it — on precisely the paths that only
execute when something has already gone wrong.

This is not hypothetical. It has been found four times: `io.open` leaking its canonical
path, `os.remove` and `os.rename` leaking theirs, and a ranged `file:write()` leaking its
whole payload at whatever size the script chose.

Every backend call can raise — a spent quota, a torn-down FileSystem, a method the
interface promises and the class lacks. Three ways out:

1. **`luaext_vfs_anchor_string()`** — hands the string to Lua's collector, which reclaims
   it however the frame leaves. Preferred: it cannot be got wrong later.
2. **Release before the raise**, on every exit. Fragile in a function with many exits.
3. **`LUAEXT_NO_RAISE_BEGIN/END`** — brackets a region that must not raise, and asserts it
   in debug builds.

`tools/check-banned-idioms.sh` refuses the specific shape that caused the last one.

### Other things worth knowing

- `LUAEXT_ASSERT`, never `ZEND_ASSERT` — see [the debug build](#the-debug-build).
- A function table is keyed **lowercase**. Use `zend_hash_str_find_ptr_lc()`, which also
  avoids building a lowered copy to throw away; the version that allocated one leaked it
  on every VFS call.
- Anything released from a Lua `__gc` should be allocated **persistently**: `__gc` also
  runs from `lua_close()` during request shutdown.
- The watchdog (`luaext_watchdog.c`) may not include `php.h` or `lua.h`. It runs on a
  process-wide thread and must not be able to reach a `zend_object` even by accident.
  `tools/check-watchdog-purity.sh` enforces it structurally.

Formatting is `clang-format`'s job — `make format` before you commit.

## Commits

**Conventional commits, subject line only.** No body, no bullet list, no co-author
trailer, and no module or file names in the subject.

```
fix: keep a canonical path alive through the error that unwinds past it
test: check what the patched interpreter computes, not just what it exposes
```

The reasoning belongs in a comment next to the code, where it is read at the point of
need, rather than in a message nobody scrolls back to.

## When you find a bug

Three steps, and the third is the one that makes this project what it is:

1. **Fix it.**
2. **Add the test that fails without the fix.** Run it against the unfixed build and
   confirm it fails — a regression test that never failed is a test of nothing.
3. **Make the mistake unwritable.** If the error is mechanically detectable, add a rule to
   `tools/check-banned-idioms.sh` and verify it fires on the reintroduced bug and stays
   quiet otherwise. If it is a *class* of error rather than one instance, that is when a
   new checker in `tools/` earns its place.

The same instinct applies to documentation: `tools/check-docs-api.php` exists because a
doc naming a method that no longer exists is a bug with no compiler to catch it.
