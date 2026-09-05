#!/usr/bin/env bash
#
# check-lua-upstream.sh — compare the vendored Lua version against the
# newest stable tag on the lua/lua GitHub mirror.
#
# Package managers can't see Lua (it isn't published to any registry we can
# poll), so this is the only automated way to notice a new upstream release
# — see .github/workflows/upstream-watch.yml, which runs this weekly and
# files a tracking issue when it finds something newer.
#
# Usage:
#   tools/check-lua-upstream.sh              human-readable report on stdout
#   tools/check-lua-upstream.sh --github-output   also append
#                                                  current=/latest=/outdated=
#                                                  to $GITHUB_OUTPUT
#
# Exit codes:
#   0  comparison succeeded (whether or not a newer release was found —
#      check the "outdated" value to distinguish)
#   1  comparison could not be performed (network error, unparsable
#      response, third_party/lua-* not vendored yet, etc.)
#
# Requires: curl, and either `jq` or `python3` to parse the GitHub API
# response (falls back to a plain grep/sed parse if neither is present).
#
# NOT checked here: whether the committed src/ still equals upstream plus
# third_party/lua-*/patches/*.patch. That is tools/vendor-lua.sh --check, which
# applies every patch in the directory in name order and diffs the result --
# so a new patch needs no registration anywhere, and one that stops applying is
# a failure there rather than a silent divergence. If you came here looking for
# a list of expected patches, there deliberately is not one: a list is a thing
# that goes stale, and the directory listing cannot.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GITHUB_OUTPUT_MODE=0
if [ "${1:-}" = "--github-output" ]; then
	GITHUB_OUTPUT_MODE=1
fi

fail() {
	echo "check-lua-upstream: $*" >&2
	exit 1
}

# --- Determine the currently vendored version -----------------------------
#
# third_party/lua-5.5.1/ is created and maintained by tools/vendor-lua.sh
# (tools/vendor-lua.sh does that; this script only reports what is available).
# We read the version from the vendored directory's own name
# (third_party/lua-X.Y.Z/) rather than parsing UPSTREAM_VERSION's internal
# format, since the directory name is the one thing the repo layout
# guarantees.
THIRD_PARTY_DIR="$REPO_ROOT/third_party"
if [ ! -d "$THIRD_PARTY_DIR" ]; then
	fail "third_party/ does not exist yet (nothing vendored to check)."
fi

CURRENT_DIR="$(find "$THIRD_PARTY_DIR" -maxdepth 1 -type d -name 'lua-*' -print -quit)"
if [ -z "$CURRENT_DIR" ]; then
	fail "no third_party/lua-* directory found (Lua not vendored yet)."
fi

CURRENT_VERSION="$(basename "$CURRENT_DIR" | sed 's/^lua-//')"
if [ -z "$CURRENT_VERSION" ]; then
	fail "could not parse a version out of $(basename "$CURRENT_DIR")."
fi

# --- Fetch the newest stable tag from lua/lua ------------------------------
#
# lua/lua tags stable releases as vX.Y.Z and pre-releases as vX.Y-beta /
# vX.Y-alpha / vX.Y-wN (see https://github.com/lua/lua/tags) — filter those
# out so a beta doesn't trigger a false "newer version available".
API_URL="https://api.github.com/repos/lua/lua/tags?per_page=100"
AUTH_HEADER=()
if [ -n "${GH_TOKEN:-${GITHUB_TOKEN:-}}" ]; then
	AUTH_HEADER=(-H "Authorization: Bearer ${GH_TOKEN:-$GITHUB_TOKEN}")
fi

RESPONSE="$(curl -fsSL "${AUTH_HEADER[@]}" -H "Accept: application/vnd.github+json" "$API_URL")" ||
	fail "failed to fetch tags from $API_URL"

if command -v jq >/dev/null 2>&1; then
	LATEST_VERSION="$(echo "$RESPONSE" | jq -r '.[].name' |
		grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' |
		sed 's/^v//' |
		sort -t. -k1,1n -k2,2n -k3,3n |
		tail -n1)"
elif command -v python3 >/dev/null 2>&1; then
	LATEST_VERSION="$(echo "$RESPONSE" | python3 -c '
import json, re, sys
tags = json.load(sys.stdin)
stable = [t["name"][1:] for t in tags if re.fullmatch(r"v\d+\.\d+\.\d+", t["name"])]
stable.sort(key=lambda v: tuple(int(part) for part in v.split(".")))
print(stable[-1] if stable else "")
')"
else
	LATEST_VERSION="$(echo "$RESPONSE" | grep -oE '"name": *"v[0-9]+\.[0-9]+\.[0-9]+"' |
		sed -E 's/.*"v([0-9]+\.[0-9]+\.[0-9]+)".*/\1/' |
		sort -t. -k1,1n -k2,2n -k3,3n |
		tail -n1)"
fi

if [ -z "${LATEST_VERSION:-}" ]; then
	fail "could not determine the latest stable lua/lua tag from the API response."
fi

# --- Compare ----------------------------------------------------------------
OUTDATED=false
if [ "$CURRENT_VERSION" != "$LATEST_VERSION" ]; then
	HIGHEST="$(printf '%s\n%s\n' "$CURRENT_VERSION" "$LATEST_VERSION" |
		sort -t. -k1,1n -k2,2n -k3,3n | tail -n1)"
	if [ "$HIGHEST" = "$LATEST_VERSION" ]; then
		OUTDATED=true
	fi
fi

echo "vendored Lua version : $CURRENT_VERSION"
echo "latest lua/lua tag   : $LATEST_VERSION"
if [ "$OUTDATED" = true ]; then
	echo "status               : OUTDATED — a newer stable Lua release is available"
else
	echo "status               : up to date"
fi

if [ "$GITHUB_OUTPUT_MODE" -eq 1 ] && [ -n "${GITHUB_OUTPUT:-}" ]; then
	{
		echo "current=$CURRENT_VERSION"
		echo "latest=$LATEST_VERSION"
		echo "outdated=$OUTDATED"
	} >>"$GITHUB_OUTPUT"
fi

exit 0
