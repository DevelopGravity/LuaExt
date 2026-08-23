#!/usr/bin/env bash
#
# gen-stubs.sh — regenerate arginfo headers from stubs/*.stub.php.
#
# Wraps php-src's own build/gen_stub.php (pinned to a specific php-src
# commit, fetched once into a gitignored cache) instead of reimplementing
# arginfo generation, so the generated headers are byte-for-byte what a
# php-src build of the same branch would produce.
#
# Why fetch it instead of assuming it's already available: Homebrew PHP
# (and most PHP installs) do not ship build/gen_stub.php — it's a php-src
# repo tool, not something `brew install php` installs. This script is the
# thing that makes `make gen-stubs` work on a machine with only `php`
# installed, no php-src checkout.
#
# gen_stub.php self-installs its own only dependency, nikic/PHP-Parser
# (see initPhpParser()/installPhpParser() inside gen_stub.php itself: it
# downloads a pinned PHP-Parser release tarball into a sibling directory on
# first run) — no composer step is needed here.
#
# gen_stub.php always writes "<name>_arginfo.h" next to the ".stub.php" it
# was given; it has no "write output elsewhere" mode outside of its
# documentation-synopsis flags (which are unrelated to arginfo generation —
# verified by reading gen_stub.php's own argument handling). Since this
# repo's layout keeps generated headers under src/ rather than stubs/, this
# script generates in stubs/ and then copies each "*_arginfo.h" into src/
# under the same basename.
#
# TODO(verify): once stubs/*.stub.php actually exists (Wave 1, agent ②),
# confirm whether it is one consolidated stub (producing a single
# src/luaext_arginfo.h, matching the plan's repo layout literally) or
# several per-class stubs (producing several src/<name>_arginfo.h files).
# This script handles either shape as-is; only the comment/expectation
# above may need updating.
#
# Usage:
#   tools/gen-stubs.sh            regenerate stubs/*_arginfo.h, then copy
#                                  into src/
#   tools/gen-stubs.sh --check    regenerate into a scratch copy and diff
#                                  against the committed src/*_arginfo.h;
#                                  writes nothing under stubs/ or src/;
#                                  non-zero exit on drift
#
# Requires: php (any version new enough to run gen_stub.php's own syntax —
# PHP 8.1+), curl, the "tokenizer" extension (gen_stub.php refuses to run
# without it).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CACHE_DIR="$REPO_ROOT/.tools-cache/gen-stub"
STUBS_DIR="$REPO_ROOT/stubs"
SRC_DIR="$REPO_ROOT/src"

# Pinned to a specific commit on php-src's PHP-8.5 branch — NOT the moving
# branch itself — so regeneration is reproducible byte-for-byte across
# machines and CI runs. Bump this by hand alongside PHP 8.5.x point
# releases (Renovate does not track this; it isn't a `uses:` reference).
PHP_SRC_REF="0990de7a5593b842e9912942bc2be362314a42ad"
GEN_STUB_URL="https://raw.githubusercontent.com/php/php-src/${PHP_SRC_REF}/build/gen_stub.php"
GEN_STUB_PATH="$CACHE_DIR/gen_stub.php"

CHECK_MODE=0
if [ "${1:-}" = "--check" ]; then
	CHECK_MODE=1
fi

fail() {
	echo "gen-stubs: $*" >&2
	exit 1
}

command -v php >/dev/null 2>&1 || fail "php is required to run gen_stub.php."
command -v curl >/dev/null 2>&1 || fail "curl is required to fetch gen_stub.php."

if [ ! -d "$STUBS_DIR" ]; then
	fail "stubs/ does not exist yet — nothing to generate."
fi

mkdir -p "$CACHE_DIR"
if [ ! -f "$GEN_STUB_PATH" ]; then
	echo "Fetching gen_stub.php (php-src@${PHP_SRC_REF:0:12})..." >&2
	curl -fsSL -o "$GEN_STUB_PATH.tmp" "$GEN_STUB_URL"
	mv "$GEN_STUB_PATH.tmp" "$GEN_STUB_PATH"
fi

# $1: a stubs/ directory (real or scratch copy) to (re)generate in place.
run_gen_stub() {
	php "$GEN_STUB_PATH" --force-regeneration "$1"
}

# $1: source directory holding freshly generated *_arginfo.h (recursively)
# $2: destination directory to copy them into, flattened to basenames
copy_arginfo() {
	local src="$1" dst="$2"
	mkdir -p "$dst"
	find "$src" -name '*_arginfo.h' -print0 |
		while IFS= read -r -d '' generated; do
			cp "$generated" "$dst/$(basename "$generated")"
		done
}

if [ "$CHECK_MODE" -eq 0 ]; then
	run_gen_stub "$STUBS_DIR"
	copy_arginfo "$STUBS_DIR" "$SRC_DIR"
	echo "Stubs regenerated into src/."
	exit 0
fi

# --check: regenerate into a scratch copy of stubs/, then diff only the
# generated *_arginfo.h basenames against what's committed in src/ today.
# (We deliberately do not diff stubs/'s own post-generation output against
# anything — those siblings are gitignored intermediates; src/ is the only
# committed copy that matters, per the repo layout.)
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

cp -R "$STUBS_DIR" "$SCRATCH/stubs"
run_gen_stub "$SCRATCH/stubs"

STATUS=0

if [ ! -d "$SRC_DIR" ]; then
	echo "gen-stubs --check: src/ does not exist yet — nothing to compare against." >&2
	exit 1
fi

FOUND_ANY=0
while IFS= read -r -d '' generated; do
	FOUND_ANY=1
	name="$(basename "$generated")"
	if [ ! -f "$SRC_DIR/$name" ]; then
		echo "gen-stubs --check: $SRC_DIR/$name is missing (gen_stub.php would generate it)." >&2
		STATUS=1
		continue
	fi
	if ! diff -u "$SRC_DIR/$name" "$generated"; then
		STATUS=1
	fi
done < <(find "$SCRATCH/stubs" -name '*_arginfo.h' -print0)

if [ "$FOUND_ANY" -eq 0 ]; then
	echo "gen-stubs --check: no *.stub.php produced any *_arginfo.h — nothing to compare (stubs/ empty or has no stub files yet)."
fi

if [ "$STATUS" -ne 0 ]; then
	echo "gen-stubs --check: generated arginfo is out of date. Run tools/gen-stubs.sh and commit the result." >&2
else
	echo "gen-stubs --check: up to date."
fi

exit "$STATUS"
