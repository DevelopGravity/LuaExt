#!/usr/bin/env bash
#
# check-generated.sh — run every "committed file derived from something
# else" drift check in one shot.
#
# This is the single entry point lint.yml and `make gen-stubs-check` use;
# it just fans out to the individual --check-capable tools so there's one
# thing to run locally before pushing. Checks whose underlying tool
# belongs to another workstream (tools/vendor-lua.sh: vendored-Lua
# workstream; tools/audit-stdlib.php: Phase-3 library-policy workstream)
# are skipped with a warning rather than failing when that tool hasn't
# landed yet.
#
# Usage: tools/check-generated.sh
#
# Exit code: non-zero if any drift check that DID run found drift or
# failed outright.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

STATUS=0

section() {
	echo
	echo "== $1 =="
}

section "Vendored Lua tree (tools/vendor-lua.sh --check)"
if [ -x tools/vendor-lua.sh ]; then
	if ! tools/vendor-lua.sh --check; then
		STATUS=1
	fi
else
	echo "tools/vendor-lua.sh not present yet — skipping."
fi

section "Generated stubs/arginfo (tools/gen-stubs.sh --check)"
if ! tools/gen-stubs.sh --check; then
	STATUS=1
fi

section "Stdlib surface golden files (tools/audit-stdlib.php --check)"
if [ -f tools/audit-stdlib.php ]; then
	if ! php tools/audit-stdlib.php --check; then
		STATUS=1
	fi
else
	echo "tools/audit-stdlib.php not present yet (arrives in Phase 3) — skipping."
fi

echo
if [ "$STATUS" -ne 0 ]; then
	echo "check-generated: one or more generated artifacts are out of date." >&2
else
	echo "check-generated: all generated artifacts are up to date."
fi

exit "$STATUS"
