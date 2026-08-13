#!/bin/sh
set -eu

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

[ "$#" -eq 1 ] || die "Usage: $0 <apt-mirror-base-url>"

apt_mirror="${1%/}"
case "${apt_mirror}" in
    http://[A-Za-z0-9]*|https://[A-Za-z0-9]*) ;;
    *) die "APT mirror must be an HTTP or HTTPS base URL" ;;
esac
case "${apt_mirror}" in
    *[!A-Za-z0-9._:/-]*) die "APT mirror contains unsupported characters" ;;
esac

set --
for source_file in \
    /etc/apt/sources.list.d/ubuntu.sources \
    /etc/apt/sources.list
do
    if [ -f "${source_file}" ]; then
        set -- "$@" "${source_file}"
    fi
done
[ "$#" -gt 0 ] || die "No Ubuntu APT source file was found"

official_pattern='https?://(archive\.ubuntu\.com/ubuntu|security\.ubuntu\.com/ubuntu|ports\.ubuntu\.com/ubuntu-ports)/?'
official_source_found=false

for source_file in "$@"; do
    if grep -Eq "${official_pattern}" "${source_file}"; then
        official_source_found=true
        sed -E -i \
            "s#${official_pattern}#${apt_mirror}/#g" \
            "${source_file}"
    fi
done

[ "${official_source_found}" = "true" ] \
    || die "No supported Ubuntu archive, security or ports URI was found"

for source_file in "$@"; do
    if grep -Eq "${official_pattern}" "${source_file}"; then
        die "An official Ubuntu APT URI remains in ${source_file}"
    fi
done

mirror_source_found=false
for source_file in "$@"; do
    if grep -Fq "${apt_mirror}/" "${source_file}"; then
        mirror_source_found=true
    fi
done
[ "${mirror_source_found}" = "true" ] || die "APT mirror replacement was not persisted"

printf 'Configured Ubuntu APT mirror: %s\n' "${apt_mirror}"
