#!/usr/bin/env bash
#
# check-workflow-php-version.sh -- one PHP version across every workflow.
#
# ci.yml declares LUAEXT_PHP_VERSION and uses it three times. The other three
# workflows cannot: a workflow-level `env` is not visible across files, and
# `workflow_call` inputs need a literal default. So the version is spelled out
# nine more times, and nothing notices when one of them is bumped and the rest
# are not -- which shows up as a release built against a PHP the CI legs never
# tested, on the one workflow that only runs when it is too late to find out.
#
# Single-sourcing it for real would mean either a repo-level `vars.*` (silently
# empty if the setting is ever missing, and invisible to a fresh clone) or a
# composite action threaded through every setup-php step in release.yml, a file
# no push exercises. This asserts agreement instead: no workflow semantics
# change, and the drift the duplication invites is caught at lint time.
#
# Cosmetic occurrences -- comments, and step names like "Setup PHP 8.5" -- are
# deliberately not checked. They mislead nobody into building the wrong thing.
#
# Usage: tools/check-workflow-php-version.sh

set -euo pipefail

cd "$(dirname "$0")/.."

readonly PROGRAM_NAME="${0##*/}"
readonly WORKFLOW_DIR=".github/workflows"
readonly CANONICAL_FILE="${WORKFLOW_DIR}/ci.yml"
readonly CANONICAL_KEY="LUAEXT_PHP_VERSION"

canonical=$(
	awk -v key="$CANONICAL_KEY" '
		$1 == key ":" || $1 == key":" {
			value = $2
			gsub(/^['"'"'"]|['"'"'"]$/, "", value)
			print value
			exit
		}
	' "$CANONICAL_FILE"
)

if [ -z "$canonical" ]; then
	printf '%s: no %s in %s -- the canonical version has moved or been renamed.\n' \
		"$PROGRAM_NAME" "$CANONICAL_KEY" "$CANONICAL_FILE" >&2
	exit 2
fi

echo "check-workflow-php-version: canonical ${CANONICAL_KEY} = '${canonical}'"

# Every literal PHP version a workflow actually acts on, as file:line:value.
# Two shapes carry one:
#
#   php-version: '8.5'          a setup-php step, or a matrix entry
#   php-version-list: '8.5'     php/php-windows-builder's extension-matrix
#   php-version-list:           a workflow_call input, whose
#     default: '8.5'            default is the version if no caller overrides
#
# Anything interpolated (${{ ... }}) is already single-sourced and is skipped.
findings=$(
	awk -F: '
		FNR == 1 { in_version_input = 0 }

		{
			line = $0
			sub(/#.*$/, "", line)
		}

		# A php-version key with the value on the same line.
		line ~ /^[[:space:]]*php-version(-list)?:[[:space:]]*[^[:space:]]/ {
			value = line
			sub(/^[^:]*:[[:space:]]*/, "", value)
			gsub(/^['"'"'"]|['"'"'"][[:space:]]*$/, "", value)
			gsub(/[[:space:]]+$/, "", value)

			if (value !~ /\$\{\{/ && value != "") {
				printf "%s:%d:%s\n", FILENAME, FNR, value
			}

			in_version_input = 0
			next
		}

		# A bare php-version-list key opens a workflow_call input block; its
		# default, a few lines down, is the effective version. Record the key
		# indent: description/type/required are siblings of default and must
		# not be mistaken for the end of the block.
		line ~ /^[[:space:]]*php-version(-list)?:[[:space:]]*$/ {
			in_version_input = 1
			indent = match(line, /[^[:space:]]/) - 1
			next
		}

		in_version_input && line ~ /^[[:space:]]*default:[[:space:]]*[^[:space:]]/ {
			value = line
			sub(/^[^:]*:[[:space:]]*/, "", value)
			gsub(/^['"'"'"]|['"'"'"][[:space:]]*$/, "", value)
			gsub(/[[:space:]]+$/, "", value)

			if (value !~ /\$\{\{/ && value != "") {
				printf "%s:%d:%s\n", FILENAME, FNR, value
			}

			in_version_input = 0
			next
		}

		# Only a key at or outside the opening indent ends the block -- a
		# sibling input, or the end of the inputs map.
		in_version_input && line ~ /^[[:space:]]*[a-zA-Z_-]+:/ &&
			(match(line, /[^[:space:]]/) - 1) <= indent { in_version_input = 0 }
	' "$WORKFLOW_DIR"/*.yml
)

status=0
checked=0

while IFS= read -r finding; do
	[ -n "$finding" ] || continue

	value="${finding##*:}"
	where="${finding%:*}"
	checked=$((checked + 1))

	if [ "$value" != "$canonical" ]; then
		printf '%s: %s pins PHP %s, but %s says %s\n' \
			"$PROGRAM_NAME" "$where" "$value" "$CANONICAL_KEY" "$canonical" >&2
		status=1
	fi
done <<-EOF
	$findings
EOF

if [ "$checked" -eq 0 ]; then
	printf '%s: found no php-version literals at all -- the extractor has gone stale.\n' \
		"$PROGRAM_NAME" >&2
	exit 2
fi

if [ "$status" -ne 0 ]; then
	printf '\nBump every workflow together, or the release builds against a PHP\nthe CI legs never tested.\n' >&2
	exit 1
fi

echo "check-workflow-php-version: ok -- ${checked} literal(s), all '${canonical}'"
