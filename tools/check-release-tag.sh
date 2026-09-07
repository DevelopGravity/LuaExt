#!/usr/bin/env bash
#
# check-release-tag.sh — decide whether a tag may become a release.
#
# Three separate questions, because the answers genuinely differ:
#
#   1. Is it semver? The regex is the one published at semver.org, unmodified
#      apart from translation to POSIX ERE.
#   2. Can Packagist publish it as that version? composer/semver implements a
#      strict subset of semver, and the gap is silent in both directions.
#   3. Does any existing tag already claim the same version?
#
# Then the version baked into the extension header has to agree with it.
#
# Usage:
#   tools/check-release-tag.sh <tag> [version-header]
#
# The header defaults to src/php_luaext.h. Set LUAEXT_TAG_LIST to a
# newline-separated list of existing tags to check collisions against a
# specific set rather than the repository's own.

set -euo pipefail

TAG="${1:-}"
VERSION_HEADER="${2:-src/php_luaext.h}"

# GitHub renders ::error:: annotations; a plain checkout just sees the text.
fail() {
	if [ -n "${GITHUB_ACTIONS:-}" ]; then
		printf '::error::%s\n' "$1" >&2
	else
		printf 'check-release-tag: %s\n' "$1" >&2
	fi
	shift
	for line in "$@"; do
		printf '  %s\n' "$line" >&2
	done
	exit 1
}

if [ -z "$TAG" ]; then
	fail "No tag given."
fi

# Plain semver, per this project's convention. Composer strips a leading "v"
# and would treat both spellings as one version, so allowing it would mean two
# tags for one release.
if [ "${TAG#v}" != "$TAG" ]; then
	fail "Tag '$TAG' has a 'v' prefix." \
		"This project's tags are plain semver: 1.2.0, not v1.2.0."
fi

# ---------------------------------------------------------------------------
# 1. Semver 2.0.0, exactly as specified.
#
# Transcribed from the suggested regex at semver.org and translated to POSIX
# ERE: bash's =~ has no \d and no (?:) groups. Nothing else is changed -- the
# pre-release and build-metadata grammars are the spec's, including the rules
# that forbid leading zeros in numeric identifiers.
# ---------------------------------------------------------------------------
SEMVER_ID='(0|[1-9][0-9]*|[0-9]*[a-zA-Z-][0-9a-zA-Z-]*)'
SEMVER_CORE='(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)'
SEMVER_PRERELEASE="(-${SEMVER_ID}(\\.${SEMVER_ID})*)?"
SEMVER_BUILD='(\+([0-9a-zA-Z-]+(\.[0-9a-zA-Z-]+)*))?'

if ! [[ "$TAG" =~ ^${SEMVER_CORE}${SEMVER_PRERELEASE}${SEMVER_BUILD}$ ]]; then
	fail "Tag '$TAG' is not a valid semver 2.0.0 version." \
		"Expected MAJOR.MINOR.PATCH, optionally -<pre-release> and +<build>." \
		"Numeric identifiers may not carry leading zeros."
fi

# ---------------------------------------------------------------------------
# 2. Publishable by Packagist.
#
# Being valid semver is not enough, and the failure is silent: Packagist runs
# composer/semver, which parses a strict subset. Measured against semver.org's
# own examples, `1.0.0-alpha.beta`, `1.0.0-0.3.7`, `1.0.0-x.7.z.92` and
# `1.0.0-x-y-z.--` are all valid semver and all rejected outright -- Packagist
# drops the tag while every release job still reports success, leaving a
# published release nobody can install and nothing anywhere saying so.
#
# Two more traps in the other direction. Build metadata parses and is then
# discarded, so `1.0.0+build` and `1.0.0` are one version. And `-p`, `-pl` and
# `-patch` are read as a patch LEVEL rather than a pre-release, so they come
# out STABLE and would become the recommended install.
#
# What is left is alpha, beta and rc. This mirrors composer/semver's
# VersionParser::$modifierRegex, which has been stable for years.
#
# `-dev` is excluded on top of that, and by choice rather than by Composer:
# it parses, but carries dev stability, which no default minimum-stability
# resolves. Attaching Windows DLLs to a release nobody can install by default
# is not something worth letting through.
#
# Matched against a lowercased copy, because semver is case-sensitive and
# Composer is not: `1.0.0-RC.1` is a perfectly good tag on its own. It is only
# ambiguous next to `1.0.0-rc.1`, which is check 3's job, not this one's.
# ---------------------------------------------------------------------------
PUBLISHABLE_PRERELEASE='(-(alpha|beta|rc)(\.?(0|[1-9][0-9]*))?)?'
TAG_LOWER="$(printf '%s' "$TAG" | tr '[:upper:]' '[:lower:]')"

if ! [[ "$TAG_LOWER" =~ ^${SEMVER_CORE}${PUBLISHABLE_PRERELEASE}$ ]]; then
	fail "Tag '$TAG' is valid semver, but Packagist cannot publish it as that version." \
		"Pre-releases must be -alpha, -beta or -rc, optionally numbered (1.2.0-rc.1)." \
		"Build metadata is dropped by Composer; -p/-pl/-patch read as stable; and" \
		"-dev would publish a release no default minimum-stability can install."
fi

# ---------------------------------------------------------------------------
# 3. No existing tag already means the same thing.
#
# Composer folds case and the separator before a pre-release number, so
# `1.0.0-rc.1`, `1.0.0-rc1` and `1.0.0-RC.1` are one version. All three are
# legitimate semver, so the shape checks above cannot catch it -- and once two
# such tags exist, one of them is unreachable and neither can be moved.
# ---------------------------------------------------------------------------
# Reduce a tag to what Composer would call it. Deliberately not sed: `\|`
# alternation in a BRE is a GNU extension, so the BSD sed on macOS would match
# nothing and silently agree that no tags collide.
canonical_version() {
	local version
	version="$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')"

	# Composer strips a leading "v" and ignores the separator before a
	# pre-release number, so v1.0.0, 1.0.0-rc.1 and 1.0.0-rc1 all fold.
	version="${version#v}"
	version="${version/-alpha./-alpha}"
	version="${version/-beta./-beta}"
	version="${version/-rc./-rc}"

	printf '%s' "$version"
}

WANTED="$(canonical_version "$TAG")"

if [ -n "${LUAEXT_TAG_LIST:-}" ]; then
	EXISTING="$LUAEXT_TAG_LIST"
elif git rev-parse --git-dir >/dev/null 2>&1; then
	EXISTING="$(git tag --list)"
else
	EXISTING=""
fi

while IFS= read -r other; do
	[ -z "$other" ] && continue
	[ "$other" = "$TAG" ] && continue

	if [ "$(canonical_version "$other")" = "$WANTED" ]; then
		fail "Tag '$TAG' and existing tag '$other' are the same version to Composer." \
			"Both normalise to the same release, so one of them would be unreachable."
	fi
done <<<"$EXISTING"

# ---------------------------------------------------------------------------
# 4. The binary agrees with the tag.
# ---------------------------------------------------------------------------
if [ ! -f "$VERSION_HEADER" ]; then
	fail "$VERSION_HEADER not found."
fi

# The header declares exactly `#define PHP_LUAEXT_VERSION "X.Y.Z"`.
HEADER_VERSION="$(sed -n 's/^#define PHP_LUAEXT_VERSION "\(.*\)"$/\1/p' "$VERSION_HEADER")"

if [ -z "$HEADER_VERSION" ]; then
	fail "Could not find PHP_LUAEXT_VERSION in $VERSION_HEADER."
fi

if [ "$HEADER_VERSION" != "$TAG" ]; then
	fail "PHP_LUAEXT_VERSION ($HEADER_VERSION) does not match the release tag ($TAG)." \
		"Bump the macro on the commit you are tagging."
fi

echo "Release tag OK: $TAG"
