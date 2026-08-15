#!/bin/sh

DEFAULT_DEPS_VERSION="ubuntu24.04-vcpkg2025.07.25-r2"
DEFAULT_BUILD_BASE_REPOSITORY="3d-tiles-clip-worker-build-base"
DEFAULT_RUNTIME_BASE_REPOSITORY="3d-tiles-clip-worker-runtime-base"
DEFAULT_MIRROR_PROFILE="official"
OFFICIAL_BASE_IMAGE="ubuntu:24.04"
OFFICIAL_VCPKG_REPOSITORY="https://github.com/microsoft/vcpkg.git"
OFFICIAL_VCPKG_ASSET_PREFIX="https://github.com"
OFFICIAL_AMD64_APT_MIRROR=""
OFFICIAL_ARM64_APT_MIRROR=""
CN_BASE_IMAGE="m.daocloud.io/docker.io/library/ubuntu:24.04@sha256:561618e2c15bf2397621dd04f96926663a3b5616c189cf7e38db7e82f5c538ea"
CN_VCPKG_REPOSITORY="https://gh-proxy.com/https://github.com/microsoft/vcpkg.git"
CN_VCPKG_ASSET_PREFIX="https://gh-proxy.com/https://github.com"
CN_VCPKG_GITHUB_ASSET_PREFIX="https://gh-proxy.com/"
CN_VCPKG_SQLITE_MIRROR_PREFIX="https://mirrors.aliyun.com/macports/distfiles/sqlite3/"
CN_AMD64_APT_MIRROR="http://mirrors.aliyun.com/ubuntu"
CN_ARM64_APT_MIRROR="http://mirrors.aliyun.com/ubuntu-ports"
VCPKG_MANIFEST_LABEL="com.justai.clip-worker.vcpkg.manifest-sha256"
DEPS_VERSION_LABEL="com.justai.clip-worker.dependencies.version"
DEPS_ARCH_LABEL="com.justai.clip-worker.dependencies.architecture"

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "Required command not found: $1"
}

require_usable_docker_builder() {
    if [ "${DOCKER_BUILDKIT:-}" = "1" ] \
        && ! docker buildx version >/dev/null 2>&1; then
        die "DOCKER_BUILDKIT=1 is set, but Docker buildx is unavailable; remove DOCKER_BUILDKIT=1 to use the supported classic builder path"
    fi
}

detect_native_arch() {
    machine_arch="$(uname -m)"
    case "${machine_arch}" in
        x86_64) printf '%s\n' "amd64" ;;
        aarch64|arm64) printf '%s\n' "arm64" ;;
        *) die "Unsupported native architecture: ${machine_arch}" ;;
    esac
}

validate_token() {
    token_name="$1"
    token_value="$2"
    case "${token_value}" in
        ''|*[!A-Za-z0-9._-]*) die "Invalid ${token_name}: ${token_value}" ;;
    esac
}

validate_image_reference() {
    image_name="$1"
    image_value="$2"
    case "${image_value}" in
        ''|*[!A-Za-z0-9._/:@-]*) die "Invalid ${image_name}: ${image_value}" ;;
    esac
}

validate_download_url() {
    url_name="$1"
    url_value="$2"
    case "${url_value}" in
        http://[A-Za-z0-9]*|https://[A-Za-z0-9]*) ;;
        *) die "Invalid ${url_name}: ${url_value}" ;;
    esac
    case "${url_value}" in
        *[!A-Za-z0-9._:/-]*) die "Invalid ${url_name}: ${url_value}" ;;
    esac
}

validate_archive_name() {
    archive_name="$1"
    case "${archive_name}" in
        ''|*/*|*\\*|*[!A-Za-z0-9._-]*) die "Invalid bundle archive name: ${archive_name}" ;;
    esac
}

sha256_file() {
    sha256sum "$1" | awk '{print $1}'
}

image_platform() {
    docker image inspect --format '{{.Os}}/{{.Architecture}}' "$1"
}

image_id() {
    docker image inspect --format '{{.Id}}' "$1"
}

image_label() {
    docker image inspect --format "{{ index .Config.Labels \"$2\" }}" "$1"
}

assert_image_platform() {
    inspected_platform="$(image_platform "$1")"
    expected_platform="linux/$2"
    [ "${inspected_platform}" = "${expected_platform}" ] \
        || die "Image $1 has platform ${inspected_platform}; expected ${expected_platform}"
}

assert_image_label() {
    inspected_label="$(image_label "$1" "$2")"
    [ "${inspected_label}" = "$3" ] \
        || die "Image $1 label $2 is ${inspected_label}; expected $3"
}

manifest_value() {
    manifest_key="$1"
    manifest_file="$2"
    awk -v expected_key="${manifest_key}" '
        index($0, "=") > 0 {
            key = substr($0, 1, index($0, "=") - 1)
            if (key == expected_key) {
                count += 1
                value = substr($0, index($0, "=") + 1)
            }
        }
        END {
            if (count != 1 || value == "") {
                exit 1
            }
            print value
        }
    ' "${manifest_file}" || die "Missing or duplicate manifest key: ${manifest_key}"
}
