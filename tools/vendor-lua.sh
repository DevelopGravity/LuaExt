#!/usr/bin/env bash
#
# vendor-lua.sh -- prove that third_party/lua-<version>/src/ really is the
# pinned upstream Lua tarball with patches/*.patch applied, and nothing else.
#
# The committed src/ tree is the build input; patches/ is only an audit
# trail. This script keeps the two honest: it downloads the pinned tarball,
# verifies its sha256, applies every patch to a scratch copy, and diffs the
# result against the committed tree. Any drift -- a hand edit that never made
# it into a patch, a patch that no longer applies, a stale SOURCES list --
# exits non-zero.
#
# Usage:
#   tools/vendor-lua.sh [--check] [--tarball FILE] [--keep] [--help]
#
#   --check          CI mode: only the final one-line verdict is printed.
#   --tarball FILE   Verify against a local tarball instead of downloading
#                    (the sha256 is still enforced).
#   --keep           Leave the scratch directory behind for inspection.
#
# Portable to macOS (BSD userland) and Linux; needs bash, tar, patch, diff
# and one of curl/wget plus one of shasum/sha256sum/openssl.

set -euo pipefail

readonly PROGRAM_NAME="${0##*/}"

SCRIPT_DIRECTORY="$(cd -- "$(dirname -- "$0")" && pwd)"
readonly SCRIPT_DIRECTORY
REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIRECTORY}/.." && pwd)"
readonly REPOSITORY_ROOT

# Upstream translation units that are deliberately never compiled into the
# extension; kept in sync with the note at the top of SOURCES.
readonly EXCLUDED_SOURCES_PATTERN='^(lua|luac|linit|liolib|loadlib|loslib)\.c$'

check_mode=0
keep_scratch=0
local_tarball=""
scratch_directory=""

# ---------------------------------------------------------------- output --

fail() {
    printf '%s: error: %s\n' "${PROGRAM_NAME}" "$*" >&2
    exit 1
}

# Progress detail; suppressed by --check so CI logs stay to one line.
report() {
    if [ "${check_mode}" -eq 0 ]; then
        printf '%s\n' "$*"
    fi
}

# The verdict; always printed.
summary() {
    printf '%s\n' "$*"
}

usage() {
    awk '
        NR == 1 { next }
        /^#/ {
            sub(/^#[[:space:]]?/, "")
            if (!started && $0 == "") next
            started = 1
            print
            next
        }
        { exit }
    ' "$0"
}

cleanup() {
    if [ -z "${scratch_directory}" ]; then
        return
    fi
    if [ "${keep_scratch}" -eq 1 ]; then
        printf 'scratch directory kept at %s\n' "${scratch_directory}"
    else
        rm -rf -- "${scratch_directory}"
    fi
}

# ----------------------------------------------------------- small tools --

# Print the sha256 of a file using whichever tool this platform ships.
sha256_of_file() {
    local file_path="$1"

    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 -- "${file_path}" | awk '{print $1}'
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum -- "${file_path}" | awk '{print $1}'
    elif command -v openssl >/dev/null 2>&1; then
        openssl dgst -sha256 "${file_path}" | awk '{print $NF}'
    else
        fail "no sha256 tool found (need shasum, sha256sum or openssl)"
    fi
}

# Read one 'key=value' setting out of an UPSTREAM_VERSION style file.
setting_from_file() {
    local settings_file="$1" wanted_key="$2"

    sed -e 's/#.*$//' -- "${settings_file}" |
        awk -F= -v key="${wanted_key}" \
            '$1 == key { sub(/^[^=]*=/, ""); print; exit }'
}

# Emit the file names listed in a SOURCES file (comments and blanks dropped).
sources_entries() {
    local sources_file="$1"

    sed -e 's/[[:space:]]*$//' -- "${sources_file}" |
        grep -v -E '^[[:space:]]*(#|$)' |
        sort
}

download_tarball() {
    local url="$1" destination="$2"

    if command -v curl >/dev/null 2>&1; then
        curl --fail --silent --show-error --location -o "${destination}" \
            -- "${url}"
    elif command -v wget >/dev/null 2>&1; then
        wget --quiet -O "${destination}" -- "${url}"
    else
        fail "no downloader found (need curl or wget); pass --tarball FILE"
    fi
}

# ---------------------------------------------------------------- checks --

# Locate the single third_party/lua-*/ directory this repository vendors.
find_vendor_directory() {
    local candidate found_directory="" found_count=0

    for candidate in "${REPOSITORY_ROOT}"/third_party/lua-*/; do
        [ -d "${candidate}" ] || continue
        found_directory="${candidate%/}"
        found_count=$((found_count + 1))
    done

    if [ "${found_count}" -ne 1 ]; then
        fail "expected exactly one third_party/lua-*/ directory, found ${found_count}"
    fi

    printf '%s' "${found_directory}"
}

# SOURCES must list exactly src/*.c minus the excluded upstream front ends.
verify_sources_list() {
    local vendor_directory="$1"
    local sources_file="${vendor_directory}/SOURCES"
    local source_file base_name expected_list listed_list listed_count

    [ -f "${sources_file}" ] || fail "missing ${sources_file}"

    expected_list=""
    for source_file in "${vendor_directory}"/src/*.c; do
        base_name="${source_file##*/}"
        if printf '%s\n' "${base_name}" |
            grep -q -E "${EXCLUDED_SOURCES_PATTERN}"; then
            continue
        fi
        expected_list="${expected_list}${base_name}
"
    done
    expected_list="$(printf '%s' "${expected_list}" | sort)"
    listed_list="$(sources_entries "${sources_file}")"

    if [ "${expected_list}" != "${listed_list}" ]; then
        printf '%s: error: SOURCES does not match src/*.c\n' \
            "${PROGRAM_NAME}" >&2
        diff -u <(printf '%s\n' "${expected_list}") \
            <(printf '%s\n' "${listed_list}") >&2 || true
        return 1
    fi

    listed_count="$(printf '%s\n' "${listed_list}" | wc -l | tr -d '[:space:]')"
    report "SOURCES ok: ${listed_count} translation units"
    return 0
}

# ------------------------------------------------------------------ main --

parse_arguments() {
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --check) check_mode=1 ;;
            --keep) keep_scratch=1 ;;
            --tarball)
                [ "$#" -ge 2 ] || fail "--tarball needs a file argument"
                local_tarball="$2"
                shift
                ;;
            --tarball=*) local_tarball="${1#--tarball=}" ;;
            -h | --help)
                usage
                exit 0
                ;;
            *) fail "unknown argument '$1' (try --help)" ;;
        esac
        shift
    done
}

main() {
    local vendor_directory version url expected_sha tarball_prefix
    local tarball_path actual_sha patch_file rebuilt_directory
    local patch_count=0

    parse_arguments "$@"

    vendor_directory="$(find_vendor_directory)"

    [ -f "${vendor_directory}/UPSTREAM_VERSION" ] ||
        fail "missing ${vendor_directory}/UPSTREAM_VERSION"
    [ -d "${vendor_directory}/src" ] ||
        fail "missing ${vendor_directory}/src"

    version="$(setting_from_file "${vendor_directory}/UPSTREAM_VERSION" version)"
    url="$(setting_from_file "${vendor_directory}/UPSTREAM_VERSION" url)"
    expected_sha="$(setting_from_file "${vendor_directory}/UPSTREAM_VERSION" sha256)"
    tarball_prefix="$(setting_from_file "${vendor_directory}/UPSTREAM_VERSION" tarball_prefix)"

    [ -n "${version}" ] || fail "UPSTREAM_VERSION has no 'version' key"
    [ -n "${url}" ] || fail "UPSTREAM_VERSION has no 'url' key"
    [ -n "${expected_sha}" ] || fail "UPSTREAM_VERSION has no 'sha256' key"
    : "${tarball_prefix:=lua-${version}}"

    if [ "${vendor_directory##*/}" != "lua-${version}" ]; then
        fail "directory ${vendor_directory##*/} does not match pinned version ${version}"
    fi

    verify_sources_list "${vendor_directory}" || return 1

    scratch_directory="$(mktemp -d "${TMPDIR:-/tmp}/vendor-lua.XXXXXXXX")"
    trap cleanup EXIT

    if [ -n "${local_tarball}" ]; then
        [ -f "${local_tarball}" ] || fail "no such tarball: ${local_tarball}"
        tarball_path="${local_tarball}"
        report "using local tarball ${tarball_path}"
    else
        tarball_path="${scratch_directory}/${tarball_prefix}.tar.gz"
        report "downloading ${url}"
        download_tarball "${url}" "${tarball_path}" ||
            fail "download failed: ${url}"
    fi

    actual_sha="$(sha256_of_file "${tarball_path}")"
    if [ "${actual_sha}" != "${expected_sha}" ]; then
        printf '%s: error: sha256 mismatch for %s\n' \
            "${PROGRAM_NAME}" "${tarball_path}" >&2
        printf '  expected %s\n  actual   %s\n' \
            "${expected_sha}" "${actual_sha}" >&2
        return 1
    fi
    report "sha256 ok: ${actual_sha}"

    tar -x -z -f "${tarball_path}" -C "${scratch_directory}" ||
        fail "could not extract ${tarball_path}"

    rebuilt_directory="${scratch_directory}/${tarball_prefix}"
    [ -d "${rebuilt_directory}/src" ] ||
        fail "tarball does not contain ${tarball_prefix}/src"

    if [ -d "${vendor_directory}/patches" ]; then
        for patch_file in "${vendor_directory}"/patches/*.patch; do
            [ -f "${patch_file}" ] || continue
            patch_count=$((patch_count + 1))
            report "applying ${patch_file##*/}"
            if ! (cd -- "${rebuilt_directory}" &&
                patch -p1 --quiet --forward -i "${patch_file}"); then
                printf '%s: error: %s does not apply cleanly\n' \
                    "${PROGRAM_NAME}" "${patch_file##*/}" >&2
                return 1
            fi
        done
    fi
    report "applied ${patch_count} patch(es)"

    if ! diff -r -u -- "${rebuilt_directory}/src" \
        "${vendor_directory}/src" >"${scratch_directory}/drift.diff"; then
        printf '%s: error: committed src/ does not match upstream + patches\n' \
            "${PROGRAM_NAME}" >&2
        printf '%s: hint: fold the difference below into patches/, or revert\n' \
            "${PROGRAM_NAME}" >&2
        printf '%s:       it from src/\n' "${PROGRAM_NAME}" >&2
        cat -- "${scratch_directory}/drift.diff" >&2
        return 1
    fi

    summary "vendor-lua: ok -- src/ == lua-${version} + ${patch_count} patch(es), sha256 verified"
    return 0
}

main "$@"
