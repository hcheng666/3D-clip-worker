#!/bin/sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/../.." && pwd)"
. "${SCRIPT_DIR}/common.sh"

usage() {
    printf 'Usage: %s <final-image-tag>\n' "$0" >&2
}

[ "$#" -eq 1 ] || {
    usage
    exit 2
}

require_command docker
require_command sha256sum
require_command awk
require_usable_docker_builder

FINAL_IMAGE="$1"
NATIVE_ARCH="$(detect_native_arch)"
DEPS_VERSION="${CLIP_WORKER_DEPS_VERSION:-${DEFAULT_DEPS_VERSION}}"
BUILD_BASE_REPOSITORY="${CLIP_WORKER_BUILD_BASE_REPOSITORY:-${DEFAULT_BUILD_BASE_REPOSITORY}}"
RUNTIME_BASE_REPOSITORY="${CLIP_WORKER_RUNTIME_BASE_REPOSITORY:-${DEFAULT_RUNTIME_BASE_REPOSITORY}}"
BUILD_BASE_IMAGE="${CLIP_WORKER_BUILD_BASE_IMAGE:-${BUILD_BASE_REPOSITORY}:${DEPS_VERSION}-${NATIVE_ARCH}}"
RUNTIME_BASE_IMAGE="${CLIP_WORKER_RUNTIME_BASE_IMAGE:-${RUNTIME_BASE_REPOSITORY}:${DEPS_VERSION}-${NATIVE_ARCH}}"
PROJECT_MANIFEST_SHA256="$(sha256_file "${PROJECT_ROOT}/vcpkg.json")"

validate_token "dependency version" "${DEPS_VERSION}"
validate_image_reference "final image" "${FINAL_IMAGE}"
validate_image_reference "build base image" "${BUILD_BASE_IMAGE}"
validate_image_reference "runtime base image" "${RUNTIME_BASE_IMAGE}"

assert_image_platform "${BUILD_BASE_IMAGE}" "${NATIVE_ARCH}"
assert_image_platform "${RUNTIME_BASE_IMAGE}" "${NATIVE_ARCH}"
assert_image_label "${BUILD_BASE_IMAGE}" "${VCPKG_MANIFEST_LABEL}" "${PROJECT_MANIFEST_SHA256}"
assert_image_label "${BUILD_BASE_IMAGE}" "${DEPS_VERSION_LABEL}" "${DEPS_VERSION}"
assert_image_label "${BUILD_BASE_IMAGE}" "${DEPS_ARCH_LABEL}" "${NATIVE_ARCH}"
assert_image_label "${RUNTIME_BASE_IMAGE}" "${VCPKG_MANIFEST_LABEL}" "${PROJECT_MANIFEST_SHA256}"
assert_image_label "${RUNTIME_BASE_IMAGE}" "${DEPS_VERSION_LABEL}" "${DEPS_VERSION}"
assert_image_label "${RUNTIME_BASE_IMAGE}" "${DEPS_ARCH_LABEL}" "${NATIVE_ARCH}"

printf 'Building %s without network access or cached build steps...\n' "${FINAL_IMAGE}"
docker build \
    --file "${PROJECT_ROOT}/Dockerfile.offline" \
    --platform "linux/${NATIVE_ARCH}" \
    --network none \
    --pull=false \
    --no-cache \
    --build-arg "TARGETARCH=${NATIVE_ARCH}" \
    --build-arg "BUILD_BASE_IMAGE=${BUILD_BASE_IMAGE}" \
    --build-arg "RUNTIME_BASE_IMAGE=${RUNTIME_BASE_IMAGE}" \
    --tag "${FINAL_IMAGE}" \
    "${PROJECT_ROOT}"

assert_image_platform "${FINAL_IMAGE}" "${NATIVE_ARCH}"
WORKER_UID="$(docker run --rm --network none --entrypoint /usr/bin/id "${FINAL_IMAGE}" -u)"
[ "${WORKER_UID}" = "10001" ] || die "Final image UID is ${WORKER_UID}; expected 10001"
docker run --rm --network none --entrypoint /usr/bin/test "${FINAL_IMAGE}" \
    -r /usr/local/share/proj/proj.db
docker run --rm --network none --entrypoint /bin/sh "${FINAL_IMAGE}" -c \
    'test ! -d /opt/vcpkg && ! command -v cmake >/dev/null 2>&1 && ! command -v ninja >/dev/null 2>&1 && ! command -v git >/dev/null 2>&1'

WORKER_VERSION="$(docker run --rm --network none "${FINAL_IMAGE}" --version)"
printf 'Offline image built successfully: %s\n' "${FINAL_IMAGE}"
printf 'Architecture: %s\n' "${NATIVE_ARCH}"
printf 'Worker version: %s\n' "${WORKER_VERSION}"
