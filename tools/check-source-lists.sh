#!/usr/bin/env bash
#
# check-source-lists.sh -- config.m4, config.w32 and src/ must agree.
#
# The vendored Lua file list cannot drift: config.m4 and config.w32 both read it
# from third_party/lua-5.5.1/SOURCES at build time. Our own file list has no such
# manifest -- it is spelled out twice, once per platform, and the two copies are
# maintained by hand.
#
# Getting that wrong is not a build error on either platform. Adding a file to
# config.m4 and forgetting config.w32 produces a Windows DLL that links (MSVC
# resolves nothing until link, and the missing symbols simply are not referenced
# from anywhere that would fail) but is missing a subsystem. The Unix side of the
# same mistake is worse still: macOS links extensions with `-undefined suppress`,
# so an omitted file becomes a null pointer call at runtime rather than a link
# error -- which is exactly what happened once already, and is why
# tools/check-linkage.sh exists.
#
# This is the check one level up from that: not "did every symbol resolve" but
# "does every file anyone added actually get compiled, everywhere".
#
# Usage: tools/check-source-lists.sh

set -euo pipefail

cd "$(dirname "$0")/.."

readonly PROGRAM_NAME="${0##*/}"

# What is actually on disk.
on_disk=$(find src -maxdepth 1 -name '*.c' -exec basename {} \; | sort)

# config.m4: the PHP_NEW_EXTENSION file list, entries like `src/luaext_foo.c \`.
# shellcheck disable=SC2016  # $ext_shared is m4 text to match, not a shell variable
in_m4=$(
	sed -n '/PHP_NEW_EXTENSION(\[luaext\]/,/\[\$ext_shared\]/p' config.m4 |
		grep -oE 'src/luaext[A-Za-z0-9_]*\.c|src/luaext\.c' |
		sed 's|^src/||' | sort -u
)

# config.w32: the EXTENSION() file list, entries like `src\luaext_foo.c`.
in_w32=$(
	sed -n '/EXTENSION("luaext"/,/luaext_flags)/p' config.w32 |
		grep -oE 'src\\\\luaext[A-Za-z0-9_]*\.c|src\\\\luaext\.c' |
		sed 's|^src\\\\||' | sort -u
)

status=0

report_difference() {
	local label="$1" left="$2" right="$3" left_name="$4" right_name="$5"
	local only

	only=$(comm -23 <(printf '%s\n' "$left") <(printf '%s\n' "$right"))

	if [ -n "$only" ]; then
		printf '%s: %s\n' "$PROGRAM_NAME" "$label" >&2
		while IFS= read -r name; do
			[ -n "$name" ] || continue
			printf '  %s\n' "$name" >&2
		done <<-EOF
			$only
		EOF
		printf '  (present in %s, absent from %s)\n\n' "$left_name" "$right_name" >&2
		status=1
	fi
}

report_difference "source files never compiled on Unix" \
	"$on_disk" "$in_m4" "src/" "config.m4"
report_difference "source files never compiled on Windows" \
	"$on_disk" "$in_w32" "src/" "config.w32"
report_difference "config.m4 names files that do not exist" \
	"$in_m4" "$on_disk" "config.m4" "src/"
report_difference "config.w32 names files that do not exist" \
	"$in_w32" "$on_disk" "config.w32" "src/"

count=$(printf '%s\n' "$on_disk" | grep -c . || true)

if [ "$count" -lt 1 ]; then
	printf '%s: found no .c files in src/ -- the extractor has gone stale.\n' \
		"$PROGRAM_NAME" >&2
	exit 2
fi

if [ "$status" -ne 0 ]; then
	printf 'Both build files list src/*.c by hand. Add the file to each.\n' >&2
	exit 1
fi

echo "check-source-lists: ok -- ${count} source file(s), config.m4 and config.w32 agree"
