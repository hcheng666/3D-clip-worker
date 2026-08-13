#!/bin/sh
set -eu

source_url="$1"
destination="$2"
connect_timeout_seconds="${VCPKG_DOWNLOAD_CONNECT_TIMEOUT_SECONDS:-15}"
max_time_seconds="${VCPKG_DOWNLOAD_MAX_TIME_SECONDS:-300}"

case "${source_url}" in
    https://github.com/*|https://raw.githubusercontent.com/*)
        if [ -n "${VCPKG_GITHUB_ASSET_PREFIX:-}" ]; then
            source_url="${VCPKG_GITHUB_ASSET_PREFIX}${source_url}"
        fi
        ;;
    https://sqlite.org/*)
        if [ -n "${VCPKG_SQLITE_MIRROR_PREFIX:-}" ]; then
            source_url="${VCPKG_SQLITE_MIRROR_PREFIX}${source_url##*/}"
        fi
        ;;
esac

curl --fail --location --retry 5 --silent --show-error \
    --connect-timeout "${connect_timeout_seconds}" \
    --max-time "${max_time_seconds}" \
    "${source_url}" --output "${destination}"
