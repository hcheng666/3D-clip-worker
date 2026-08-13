#!/bin/sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/../.." && pwd)"
. "${SCRIPT_DIR}/common.sh"

usage() {
    printf 'Usage: %s <bundle-directory>\n' "$0" >&2
}

[ "$#" -eq 1 ] || {
    usage
    exit 2
}

require_command docker
require_command sha256sum
require_command awk

BUNDLE_DIR="$1"
[ -d "${BUNDLE_DIR}" ] || die "Bundle directory not found: ${BUNDLE_DIR}"
BUNDLE_DIR="$(CDPATH= cd -- "${BUNDLE_DIR}" && pwd)"
MANIFEST_FILE="${BUNDLE_DIR}/manifest.properties"

[ -f "${BUNDLE_DIR}/SHA256SUMS" ] || die "Missing SHA256SUMS"
[ -f "${MANIFEST_FILE}" ] || die "Missing manifest.properties"

printf 'Verifying bundle file checksums...\n'
(
    cd "${BUNDLE_DIR}"
    sha256sum -c SHA256SUMS
)

SCHEMA_VERSION="$(manifest_value schema_version "${MANIFEST_FILE}")"
NATIVE_ARCH="$(detect_native_arch)"
BUNDLE_ARCH="$(manifest_value architecture "${MANIFEST_FILE}")"
DEPS_VERSION="$(manifest_value dependencies_version "${MANIFEST_FILE}")"
VCPKG_MANIFEST_SHA256="$(manifest_value vcpkg_manifest_sha256 "${MANIFEST_FILE}")"
BUILD_BASE_IMAGE="$(manifest_value build_base_image "${MANIFEST_FILE}")"
BUILD_BASE_IMAGE_ID="$(manifest_value build_base_image_id "${MANIFEST_FILE}")"
BUILD_BASE_ARCHIVE="$(manifest_value build_base_archive "${MANIFEST_FILE}")"
RUNTIME_BASE_IMAGE="$(manifest_value runtime_base_image "${MANIFEST_FILE}")"
RUNTIME_BASE_IMAGE_ID="$(manifest_value runtime_base_image_id "${MANIFEST_FILE}")"
RUNTIME_BASE_ARCHIVE="$(manifest_value runtime_base_archive "${MANIFEST_FILE}")"

[ "${SCHEMA_VERSION}" = "1" ] || die "Unsupported bundle schema: ${SCHEMA_VERSION}"
validate_token "bundle architecture" "${BUNDLE_ARCH}"
validate_token "dependency version" "${DEPS_VERSION}"
validate_image_reference "build base image" "${BUILD_BASE_IMAGE}"
validate_image_reference "runtime base image" "${RUNTIME_BASE_IMAGE}"
validate_archive_name "${BUILD_BASE_ARCHIVE}"
validate_archive_name "${RUNTIME_BASE_ARCHIVE}"
[ "${BUNDLE_ARCH}" = "${NATIVE_ARCH}" ] \
    || die "Bundle architecture ${BUNDLE_ARCH} does not match native ${NATIVE_ARCH}"
[ -f "${BUNDLE_DIR}/${BUILD_BASE_ARCHIVE}" ] || die "Missing ${BUILD_BASE_ARCHIVE}"
[ -f "${BUNDLE_DIR}/${RUNTIME_BASE_ARCHIVE}" ] || die "Missing ${RUNTIME_BASE_ARCHIVE}"

PROJECT_MANIFEST_SHA256="$(sha256_file "${PROJECT_ROOT}/vcpkg.json")"
[ "${PROJECT_MANIFEST_SHA256}" = "${VCPKG_MANIFEST_SHA256}" ] \
    || die "Project vcpkg.json does not match the dependency bundle"

printf 'Loading %s...\n' "${BUILD_BASE_IMAGE}"
docker load --input "${BUNDLE_DIR}/${BUILD_BASE_ARCHIVE}"
printf 'Loading %s...\n' "${RUNTIME_BASE_IMAGE}"
docker load --input "${BUNDLE_DIR}/${RUNTIME_BASE_ARCHIVE}"

assert_image_platform "${BUILD_BASE_IMAGE}" "${NATIVE_ARCH}"
assert_image_platform "${RUNTIME_BASE_IMAGE}" "${NATIVE_ARCH}"
[ "$(image_id "${BUILD_BASE_IMAGE}")" = "${BUILD_BASE_IMAGE_ID}" ] \
    || die "Loaded build base image ID does not match the bundle manifest"
[ "$(image_id "${RUNTIME_BASE_IMAGE}")" = "${RUNTIME_BASE_IMAGE_ID}" ] \
    || die "Loaded runtime base image ID does not match the bundle manifest"
assert_image_label "${BUILD_BASE_IMAGE}" "${VCPKG_MANIFEST_LABEL}" "${VCPKG_MANIFEST_SHA256}"
assert_image_label "${BUILD_BASE_IMAGE}" "${DEPS_VERSION_LABEL}" "${DEPS_VERSION}"
assert_image_label "${BUILD_BASE_IMAGE}" "${DEPS_ARCH_LABEL}" "${NATIVE_ARCH}"
assert_image_label "${RUNTIME_BASE_IMAGE}" "${VCPKG_MANIFEST_LABEL}" "${VCPKG_MANIFEST_SHA256}"
assert_image_label "${RUNTIME_BASE_IMAGE}" "${DEPS_VERSION_LABEL}" "${DEPS_VERSION}"
assert_image_label "${RUNTIME_BASE_IMAGE}" "${DEPS_ARCH_LABEL}" "${NATIVE_ARCH}"

printf 'Offline dependency bundle loaded successfully for %s.\n' "${NATIVE_ARCH}"
printf 'CLIP_WORKER_BUILD_BASE_IMAGE=%s\n' "${BUILD_BASE_IMAGE}"
printf 'CLIP_WORKER_RUNTIME_BASE_IMAGE=%s\n' "${RUNTIME_BASE_IMAGE}"
