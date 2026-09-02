#!/usr/bin/env bash
#
# check-banned-idioms.sh -- refuse C idioms that are correct-looking and wrong.
#
# The same job as check-watchdog-purity.sh and check-linkage.sh: make a mistake
# unwritable rather than trusting everyone to remember it. Each entry below is a
# pattern that compiles cleanly, passes review, and fails somewhere expensive.
#
# ZVAL_ARR followed by an addref
# ------------------------------
# ZVAL_ARR(&z, ht) stamps the "refcounted" type flag onto a raw HashTable*
# UNCONDITIONALLY. If ht is one of PHP's shared immutable arrays -- most often
# zend_empty_array, which an empty Z_PARAM_ARRAY_HT hands you -- its refcount
# lives in read-only memory, so the next Z_ADDREF/Z_TRY_ADDREF SIGBUSes. The
# TRY_ prefix does not save you: the flag ZVAL_ARR just set is exactly what
# Z_REFCOUNTED_P tests.
#
# Use Z_PARAM_ARRAY_HT to receive and ZVAL_COPY to keep, which is what every
# call site in src/ already does. ZVAL_EMPTY_ARRAY exists for the empty case.
#
# A line may opt out with a trailing
#     /* luaext-allow: <reason> */
# which is deliberately noisy: it survives review and shows up in a grep.

set -euo pipefail

cd "$(dirname "$0")/.."

readonly PROGRAM_NAME="${0##*/}"

status=0
header_printed=0

# Each rule is three fields separated by '~' (not '|', which appears inside the
# patterns themselves): extended-regex ~ short name ~ why it is banned.
readonly RULES=(
	'ZVAL_ARR[[:space:]]*[(]~ZVAL_ARR~stamps refcounted onto a possibly-immutable HashTable; receive with Z_PARAM_ARRAY_HT and keep with ZVAL_COPY'
	'not implemented yet~unimplemented method~a declared method that throws is invisible to every documentation check, because the name resolves; either implement it or remove it from the stubs'
)

for rule in "${RULES[@]}"; do
	pattern="${rule%%~*}"
	remainder="${rule#*~}"
	name="${remainder%%~*}"
	reason="${remainder#*~}"

	# Deliberately NOT 2>/dev/null. A malformed pattern must fail loudly: the
	# first draft of this script hid an unmatched '(' behind a discarded stderr
	# and cheerfully reported every file clean, which is precisely the class of
	# silent-pass bug the whole tools/check-*.sh family exists to prevent.
	set +e
	matches=$(grep -rnE --include='*.c' --include='*.h' -e "$pattern" src/)
	grep_status=$?
	set -e

	if [ "$grep_status" -gt 1 ]; then
		printf '%s: grep failed (status %d) for pattern: %s\n' \
			"${PROGRAM_NAME}" "$grep_status" "$pattern" >&2
		exit 2
	fi

	[ -n "$matches" ] || continue

	while IFS= read -r hit; do
		[ -n "$hit" ] || continue

		case "$hit" in
		*'luaext-allow:'*) continue ;;
		esac

		if [ "$header_printed" -eq 0 ]; then
			printf '%s: banned idiom(s) found.\n\n' "${PROGRAM_NAME}" >&2
			header_printed=1
		fi

		status=1
		printf '  %s\n      %s: %s\n' "$hit" "$name" "$reason" >&2
	done <<-EOF
		$matches
	EOF
done

if [ "$status" -ne 0 ]; then
	printf '\nIf one is genuinely correct, mark that line:\n    /* luaext-allow: <reason> */\n' >&2
	exit 1
fi

echo "check-banned-idioms: ok -- no banned idioms in src/"
