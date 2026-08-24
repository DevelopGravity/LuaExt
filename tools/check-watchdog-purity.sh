#!/usr/bin/env bash
#
# check-watchdog-purity.sh — the watchdog must not be able to reach PHP.
#
# The watchdog runs on a thread PHP did not create. Under a worker SAPI like
# FrankenPHP there are many PHP threads, each with its own module globals, and
# the watchdog belongs to none of them: it has no TSRM context. A LUAEXT_G()
# from there would not fail to compile and would not crash -- it would quietly
# read some other thread's globals. That is worse than a crash, and it is not
# the kind of mistake code review reliably catches, because the line that makes
# it looks exactly like every correct line elsewhere in the extension.
#
# So the three files below are structurally PHP-free and Lua-free: they include
# neither php.h nor lua.h, and therefore cannot name a zend_object, the module
# globals, a class entry or the interpreter even by accident. This script is the
# second, independent check on that property -- the first being that the code
# would not compile if somebody removed the include and kept the usage.
#
# It is deliberately a grep and not a compiler pass: a compiler check would only
# fail once somebody had already added the include, whereas this fails on the
# first token. Comments are stripped before scanning, so the files can still
# explain what they are forbidden from doing.
#
# Escape hatch, for the case this script gets wrong:
#
#     something_php_shaped(); /* watchdog-purity: allow <reason> */
#
# The marker must be on the offending line or the line directly above it, and
# it must carry a reason. Adding one should feel like a decision.
#
# Usage:
#   tools/check-watchdog-purity.sh
#
# Exit codes:
#   0  every guarded file is clean
#   1  a forbidden token was found, or the script could not run

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# The pure files. Headers as well as sources: luaext_watchdog.h is included by
# the impure layer, and an include added there leaks into the same translation
# units that must not have it.
GUARDED_FILES=(
	"src/luaext_clock.c"
	"src/luaext_clock.h"
	"src/luaext_thread.c"
	"src/luaext_thread.h"
	"src/luaext_watchdog.c"
	"src/luaext_watchdog.h"
)

# Extended regular expressions, one per forbidden construct, with the
# explanation that belongs in the failure message rather than in a comment
# nobody reads at 3am. Format: <regex>|<explanation>
FORBIDDEN=(
	'#[[:space:]]*include[[:space:]]*[<"]php|includes a PHP header'
	'#[[:space:]]*include[[:space:]]*[<"]Zend/|includes a Zend header'
	'\bzend_|names a Zend type or function'
	'\bphp_|names a PHP type or function'
	'emalloc|uses the request allocator, which is per-thread and freed at RSHUTDOWN'
	'\bLUAEXT_G\b|reads module globals, which on this thread belong to somebody else'
	'TSRMLS|uses the thread-safe resource manager, which has no context here'
	'\bts_resource\b|reaches into TSRM, which has no context here'
	'\blua_|calls the Lua C API, which is not thread safe and is not this layer'\''s job'
	'\bluaL_|calls the Lua auxiliary library, same reason'
)

fail() {
	echo "check-watchdog-purity: $*" >&2
	exit 1
}

command -v perl >/dev/null 2>&1 ||
	fail "perl is required to strip comments before scanning."

violations=0

for relative in "${GUARDED_FILES[@]}"; do
	file="$REPO_ROOT/$relative"

	if [ ! -f "$file" ]; then
		fail "$relative does not exist; update GUARDED_FILES if it was renamed."
	fi

	# Replace every C comment with the same number of newlines it spanned, so
	# the scan sees only code and grep -n still reports real line numbers.
	stripped="$(perl -0777 -pe '
		s{/\*.*?\*/}{ my $body = $&; $body =~ s/[^\n]//g; $body }gse;
		s{//[^\n]*}{}g;
	' "$file")"

	for entry in "${FORBIDDEN[@]}"; do
		pattern="${entry%%|*}"
		explanation="${entry#*|}"

		while IFS= read -r hit; do
			[ -n "$hit" ] || continue

			line_number="${hit%%:*}"
			offending="$(sed -n "${line_number}p" "$file")"
			preceding=""

			if [ "$line_number" -gt 1 ]; then
				preceding="$(sed -n "$((line_number - 1))p" "$file")"
			fi

			# The marker is looked for in the ORIGINAL text, because it lives in
			# a comment and the scanned copy has had its comments removed.
			case "$offending$preceding" in
			*"watchdog-purity: allow "*) continue ;;
			esac

			echo "$relative:$line_number: $explanation" >&2
			echo "    ${hit#*:}" >&2
			violations=$((violations + 1))
		done < <(printf '%s\n' "$stripped" | grep -nE "$pattern" || true)
	done
done

if [ "$violations" -ne 0 ]; then
	echo >&2
	echo "check-watchdog-purity: $violations violation(s)." >&2
	echo "The watchdog thread has no TSRM context: anything it reaches through" >&2
	echo "one of these belongs to a different thread. Move the work to" >&2
	echo "src/luaext_timers.c, which is the layer allowed to touch PHP." >&2
	exit 1
fi

echo "check-watchdog-purity: ok -- ${#GUARDED_FILES[@]} file(s) are PHP-free and Lua-free"
exit 0
