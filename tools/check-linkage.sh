#!/usr/bin/env bash
#
# Fail when the built module carries an undefined luaext_* symbol.
#
# THE FAILURE THIS EXISTS FOR. Adding a source file means adding it to config.m4
# AND re-running phpize && ./configure -- the Makefile is generated, so an
# existing tree keeps building happily without the new file. On Linux the link
# would then fail and say so. On macOS it does not: PHP links shared extensions
# with `-undefined suppress -flat_namespace`, so every call into the missing
# translation unit becomes a jump through a null stub.
#
# The result is a module that compiles cleanly, loads cleanly, reports its
# version cleanly, and segfaults the moment one of those functions is called --
# which cost an hour of bisecting a crash that looked like memory corruption in
# code that was never actually compiled.
#
# An undefined luaext_* symbol is never legitimate: every one of them is defined
# in this extension. Symbols from PHP, Lua's vendored tree and libc are resolved
# by the host binary at load time and are expected to show up here, so the check
# is deliberately narrow rather than "no undefined symbols at all".

set -euo pipefail

cd "$(dirname "$0")/.."

module=""

for candidate in modules/luaext.so modules/luaext.dylib; do
	if [ -f "$candidate" ]; then
		module="$candidate"
		break
	fi
done

if [ -z "$module" ]; then
	echo "check-linkage: no built module under modules/; run make first" >&2
	exit 1
fi

if ! command -v nm >/dev/null 2>&1; then
	echo "check-linkage: skipped -- nm is not available on this platform"
	exit 0
fi

# macOS prefixes C symbols with an underscore, Linux does not; matching on
# "luaext_" alone covers both without the script needing to know which it is on.
undefined="$(nm -u "$module" 2>/dev/null | grep 'luaext_' || true)"

if [ -n "$undefined" ]; then
	echo "check-linkage: $module has undefined luaext symbols." >&2
	echo >&2
	echo "$undefined" | sed 's/^/  /' >&2
	echo >&2
	echo "A source file is missing from the build. Add it to config.m4 (and" >&2
	echo "config.w32), then re-run: phpize && ./configure && make" >&2
	exit 1
fi

echo "check-linkage: ok -- $module has no undefined luaext symbols"
