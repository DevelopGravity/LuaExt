#!/usr/bin/env bash
#
# check-generated.sh — run every "committed file derived from something
# else" drift check in one shot.
#
# This is the single entry point lint.yml and `make check` use; it fans out to
# the individual --check-capable tools so there is one thing to run.
#
# A MISSING TOOL IS A FAILURE, NOT A SKIP. Two of these used to print
# "not present yet — skipping" from a time when the scripts were still being
# written by parallel workstreams. Both have existed for many waves, so the
# branches were dead -- but what they encoded was "if this gate's script
# disappears, report success", which is the exact failure this repository has
# been burned by before (see the note on ci-required's needs: list). A gate that
# cannot run has not passed.
#
# Usage: tools/check-generated.sh
#
# Exit code: non-zero if any drift check found drift, failed outright, or could
# not be run at all.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

STATUS=0

section() {
	echo
	echo "== $1 =="
}

section "Vendored Lua tree (tools/vendor-lua.sh --check)"
if [ ! -x tools/vendor-lua.sh ]; then
	echo "tools/vendor-lua.sh is missing or not executable — this checkout is" >&2
	echo "incomplete, and the vendored Lua tree is therefore UNCHECKED." >&2
	STATUS=1
elif ! tools/vendor-lua.sh --check; then
	STATUS=1
fi

section "Generated stubs/arginfo (tools/gen-stubs.sh --check)"
if ! tools/gen-stubs.sh --check; then
	STATUS=1
fi

section "Build-file source lists (tools/check-source-lists.sh)"
if ! tools/check-source-lists.sh; then
	STATUS=1
fi

section "Stdlib surface golden files (tools/audit-stdlib.php --check)"
if [ ! -f tools/audit-stdlib.php ]; then
	echo "tools/audit-stdlib.php is missing — this checkout is incomplete, and the" >&2
	echo "stdlib surface a script can reach is therefore UNCHECKED." >&2
	STATUS=1
elif ! php tools/audit-stdlib.php --check; then
	STATUS=1
fi

echo
if [ "$STATUS" -ne 0 ]; then
	echo "check-generated: one or more generated artifacts are out of date." >&2
else
	echo "check-generated: all generated artifacts are up to date."
fi

exit "$STATUS"
