#!/bin/sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/../.." && pwd)"
. "${SCRIPT_DIR}/common.sh"

usage() {
    printf 'Usage: %s [--mirror-profile official|cn] <empty-output-directory>\n' "$0" >&2
}

MIRROR_PROFILE="${DEFAULT_MIRROR_PROFILE}"
case "$#" in
    1)
        OUTPUT_DIR="$1"
        ;;
    3)
        [ "$1" = "--mirror-profile" ] || {
            usage
            exit 2
        }
        MIRROR_PROFILE="$2"
        OUTPUT_DIR="$3"
        ;;
    *)
        usage
        exit 2
        ;;
esac

case "${MIRROR_PROFILE}" in
    official)
        PROFILE_BASE_IMAGE="${OFFICIAL_BASE_IMAGE}"
        PROFILE_VCPKG_REPOSITORY="${OFFICIAL_VCPKG_REPOSITORY}"
        PROFILE_VCPKG_ASSET_PREFIX="${OFFICIAL_VCPKG_ASSET_PREFIX}"
        PROFILE_VCPKG_GITHUB_ASSET_PREFIX=""
        PROFILE_VCPKG_SQLITE_MIRROR_PREFIX=""
        PROFILE_AMD64_APT_MIRROR="${OFFICIAL_AMD64_APT_MIRROR}"
        PROFILE_ARM64_APT_MIRROR="${OFFICIAL_ARM64_APT_MIRROR}"
        ;;
    cn)
        PROFILE_BASE_IMAGE="${CN_BASE_IMAGE}"
        PROFILE_VCPKG_REPOSITORY="${CN_VCPKG_REPOSITORY}"
        PROFILE_VCPKG_ASSET_PREFIX="${CN_VCPKG_ASSET_PREFIX}"
        PROFILE_VCPKG_GITHUB_ASSET_PREFIX="${CN_VCPKG_GITHUB_ASSET_PREFIX}"
        PROFILE_VCPKG_SQLITE_MIRROR_PREFIX="${CN_VCPKG_SQLITE_MIRROR_PREFIX}"
        PROFILE_AMD64_APT_MIRROR="${CN_AMD64_APT_MIRROR}"
        PROFILE_ARM64_APT_MIRROR="${CN_ARM64_APT_MIRROR}"
        ;;
    *)
        die "Unsupported mirror profile: ${MIRROR_PROFILE}"
        ;;
esac

require_command docker
require_command sha256sum
require_command awk
require_command find
require_command date
require_usable_docker_builder

NATIVE_ARCH="$(detect_native_arch)"
case "${NATIVE_ARCH}" in
    amd64) PROFILE_APT_MIRROR="${PROFILE_AMD64_APT_MIRROR}" ;;
    arm64) PROFILE_APT_MIRROR="${PROFILE_ARM64_APT_MIRROR}" ;;
    *) die "Unsupported native architecture: ${NATIVE_ARCH}" ;;
esac
DEPS_VERSION="${CLIP_WORKER_DEPS_VERSION:-${DEFAULT_DEPS_VERSION}}"
BUILD_BASE_REPOSITORY="${CLIP_WORKER_BUILD_BASE_REPOSITORY:-${DEFAULT_BUILD_BASE_REPOSITORY}}"
RUNTIME_BASE_REPOSITORY="${CLIP_WORKER_RUNTIME_BASE_REPOSITORY:-${DEFAULT_RUNTIME_BASE_REPOSITORY}}"
BASE_IMAGE="${CLIP_WORKER_BASE_IMAGE:-${PROFILE_BASE_IMAGE}}"
VCPKG_REF="${CLIP_WORKER_VCPKG_REF:-2025.07.25}"
VCPKG_REPOSITORY="${CLIP_WORKER_VCPKG_REPOSITORY:-${PROFILE_VCPKG_REPOSITORY}}"
VCPKG_ASSET_PREFIX="${CLIP_WORKER_VCPKG_ASSET_PREFIX:-${PROFILE_VCPKG_ASSET_PREFIX}}"
VCPKG_GITHUB_ASSET_PREFIX="${CLIP_WORKER_VCPKG_GITHUB_ASSET_PREFIX:-${PROFILE_VCPKG_GITHUB_ASSET_PREFIX}}"
VCPKG_SQLITE_MIRROR_PREFIX="${CLIP_WORKER_VCPKG_SQLITE_MIRROR_PREFIX:-${PROFILE_VCPKG_SQLITE_MIRROR_PREFIX}}"
APT_MIRROR="${CLIP_WORKER_APT_MIRROR:-${PROFILE_APT_MIRROR}}"

validate_token "dependency version" "${DEPS_VERSION}"
validate_token "architecture" "${NATIVE_ARCH}"
validate_token "mirror profile" "${MIRROR_PROFILE}"
validate_image_reference "build base repository" "${BUILD_BASE_REPOSITORY}"
validate_image_reference "runtime base repository" "${RUNTIME_BASE_REPOSITORY}"
validate_image_reference "base image" "${BASE_IMAGE}"
if [ -n "${APT_MIRROR}" ]; then
    validate_download_url "APT mirror" "${APT_MIRROR}"
fi

mkdir -p "${OUTPUT_DIR}"
if [ -n "$(find "${OUTPUT_DIR}" -mindepth 1 -maxdepth 1 -print -quit)" ]; then
    die "Output directory must be empty: ${OUTPUT_DIR}"
fi
OUTPUT_DIR="$(CDPATH= cd -- "${OUTPUT_DIR}" && pwd)"

VCPKG_MANIFEST_SHA256="$(sha256_file "${PROJECT_ROOT}/vcpkg.json")"
BUILD_BASE_IMAGE="${BUILD_BASE_REPOSITORY}:${DEPS_VERSION}-${NATIVE_ARCH}"
RUNTIME_BASE_IMAGE="${RUNTIME_BASE_REPOSITORY}:${DEPS_VERSION}-${NATIVE_ARCH}"
BUILD_BASE_ARCHIVE="build-base-${DEPS_VERSION}-${NATIVE_ARCH}.tar"
RUNTIME_BASE_ARCHIVE="runtime-base-${DEPS_VERSION}-${NATIVE_ARCH}.tar"

printf 'Mirror profile: %s\n' "${MIRROR_PROFILE}"
printf 'Base image: %s\n' "${BASE_IMAGE}"
printf 'APT mirror: %s\n' "${APT_MIRROR:-official Ubuntu sources}"
printf 'Building %s from network-resolved dependencies...\n' "${BUILD_BASE_IMAGE}"
docker build \
    --file "${PROJECT_ROOT}/Dockerfile.base" \
    --target build-base \
    --platform "linux/${NATIVE_ARCH}" \
    --pull \
    --no-cache \
    --build-arg "TARGETARCH=${NATIVE_ARCH}" \
    --build-arg "BASE_IMAGE=${BASE_IMAGE}" \
    --build-arg "DEPS_VERSION=${DEPS_VERSION}" \
    --build-arg "VCPKG_MANIFEST_SHA256=${VCPKG_MANIFEST_SHA256}" \
    --build-arg "VCPKG_REF=${VCPKG_REF}" \
    --build-arg "VCPKG_REPOSITORY=${VCPKG_REPOSITORY}" \
    --build-arg "VCPKG_ASSET_PREFIX=${VCPKG_ASSET_PREFIX}" \
    --build-arg "VCPKG_GITHUB_ASSET_PREFIX=${VCPKG_GITHUB_ASSET_PREFIX}" \
    --build-arg "VCPKG_SQLITE_MIRROR_PREFIX=${VCPKG_SQLITE_MIRROR_PREFIX}" \
    --build-arg "APT_MIRROR=${APT_MIRROR}" \
    --tag "${BUILD_BASE_IMAGE}" \
    "${PROJECT_ROOT}"

printf 'Building %s...\n' "${RUNTIME_BASE_IMAGE}"
docker build \
    --file "${PROJECT_ROOT}/Dockerfile.base" \
    --target runtime-base \
    --platform "linux/${NATIVE_ARCH}" \
    --pull \
    --build-arg "TARGETARCH=${NATIVE_ARCH}" \
    --build-arg "BASE_IMAGE=${BASE_IMAGE}" \
    --build-arg "DEPS_VERSION=${DEPS_VERSION}" \
    --build-arg "VCPKG_MANIFEST_SHA256=${VCPKG_MANIFEST_SHA256}" \
    --build-arg "VCPKG_REF=${VCPKG_REF}" \
    --build-arg "VCPKG_REPOSITORY=${VCPKG_REPOSITORY}" \
    --build-arg "VCPKG_ASSET_PREFIX=${VCPKG_ASSET_PREFIX}" \
    --build-arg "VCPKG_GITHUB_ASSET_PREFIX=${VCPKG_GITHUB_ASSET_PREFIX}" \
    --build-arg "VCPKG_SQLITE_MIRROR_PREFIX=${VCPKG_SQLITE_MIRROR_PREFIX}" \
    --build-arg "APT_MIRROR=${APT_MIRROR}" \
    --tag "${RUNTIME_BASE_IMAGE}" \
    "${PROJECT_ROOT}"

assert_image_platform "${BUILD_BASE_IMAGE}" "${NATIVE_ARCH}"
assert_image_platform "${RUNTIME_BASE_IMAGE}" "${NATIVE_ARCH}"
assert_image_label "${BUILD_BASE_IMAGE}" "${VCPKG_MANIFEST_LABEL}" "${VCPKG_MANIFEST_SHA256}"
assert_image_label "${BUILD_BASE_IMAGE}" "${DEPS_VERSION_LABEL}" "${DEPS_VERSION}"
assert_image_label "${BUILD_BASE_IMAGE}" "${DEPS_ARCH_LABEL}" "${NATIVE_ARCH}"
assert_image_label "${RUNTIME_BASE_IMAGE}" "${VCPKG_MANIFEST_LABEL}" "${VCPKG_MANIFEST_SHA256}"
assert_image_label "${RUNTIME_BASE_IMAGE}" "${DEPS_VERSION_LABEL}" "${DEPS_VERSION}"
assert_image_label "${RUNTIME_BASE_IMAGE}" "${DEPS_ARCH_LABEL}" "${NATIVE_ARCH}"

docker run --rm --network none --entrypoint /bin/sh "${BUILD_BASE_IMAGE}" -c \
    'test -x /opt/vcpkg/vcpkg && test -d /opt/clip-worker-deps/vcpkg_installed && cmake --version >/dev/null && ninja --version >/dev/null'
docker run --rm --network none --entrypoint /usr/bin/test "${RUNTIME_BASE_IMAGE}" \
    -r /usr/local/share/proj/proj.db

BUILD_BASE_IMAGE_ID="$(image_id "${BUILD_BASE_IMAGE}")"
RUNTIME_BASE_IMAGE_ID="$(image_id "${RUNTIME_BASE_IMAGE}")"

printf 'Exporting base images to %s...\n' "${OUTPUT_DIR}"
docker save --output "${OUTPUT_DIR}/${BUILD_BASE_ARCHIVE}" "${BUILD_BASE_IMAGE}"
docker save --output "${OUTPUT_DIR}/${RUNTIME_BASE_ARCHIVE}" "${RUNTIME_BASE_IMAGE}"
cp "${SCRIPT_DIR}/BUNDLE_README.md" "${OUTPUT_DIR}/README.md"

MANIFEST_FILE="${OUTPUT_DIR}/manifest.properties"
{
    printf 'schema_version=1\n'
    printf 'created_at_utc=%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    printf 'architecture=%s\n' "${NATIVE_ARCH}"
    printf 'network_profile=%s\n' "${MIRROR_PROFILE}"
    printf 'base_image=%s\n' "${BASE_IMAGE}"
    printf 'apt_mirror=%s\n' "${APT_MIRROR:-official}"
    printf 'dependencies_version=%s\n' "${DEPS_VERSION}"
    printf 'vcpkg_ref=%s\n' "${VCPKG_REF}"
    printf 'vcpkg_manifest_sha256=%s\n' "${VCPKG_MANIFEST_SHA256}"
    printf 'build_base_image=%s\n' "${BUILD_BASE_IMAGE}"
    printf 'build_base_image_id=%s\n' "${BUILD_BASE_IMAGE_ID}"
    printf 'build_base_archive=%s\n' "${BUILD_BASE_ARCHIVE}"
    printf 'runtime_base_image=%s\n' "${RUNTIME_BASE_IMAGE}"
    printf 'runtime_base_image_id=%s\n' "${RUNTIME_BASE_IMAGE_ID}"
    printf 'runtime_base_archive=%s\n' "${RUNTIME_BASE_ARCHIVE}"
} > "${MANIFEST_FILE}"

(
    cd "${OUTPUT_DIR}"
    sha256sum \
        "${BUILD_BASE_ARCHIVE}" \
        "${RUNTIME_BASE_ARCHIVE}" \
        manifest.properties \
        README.md \
        > SHA256SUMS
)

printf 'Offline dependency bundle created: %s\n' "${OUTPUT_DIR}"
printf 'Architecture: %s\n' "${NATIVE_ARCH}"
printf 'Dependency version: %s\n' "${DEPS_VERSION}"
