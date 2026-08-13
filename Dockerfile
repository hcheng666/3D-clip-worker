ARG BASE_IMAGE=ubuntu:24.04

FROM ${BASE_IMAGE} AS build

ENV DEBIAN_FRONTEND=noninteractive \
    VCPKG_DISABLE_METRICS=1 \
    VCPKG_FORCE_SYSTEM_BINARIES=1

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        build-essential ca-certificates cmake curl git ninja-build pkg-config tar unzip zip \
    && rm -rf /var/lib/apt/lists/*

ARG TARGETARCH
ARG VCPKG_REF=2025.07.25
ARG VCPKG_REPOSITORY=https://github.com/microsoft/vcpkg.git
ARG VCPKG_ASSET_PREFIX=https://github.com

RUN git clone --depth 1 --branch "${VCPKG_REF}" "${VCPKG_REPOSITORY}" /opt/vcpkg \
    && . /opt/vcpkg/scripts/vcpkg-tool-metadata.txt \
    && case "${TARGETARCH}" in \
        amd64) VCPKG_TOOL_NAME=vcpkg-glibc; VCPKG_TOOL_SHA="${VCPKG_GLIBC_SHA}" ;; \
        arm64) VCPKG_TOOL_NAME=vcpkg-glibc-arm64; VCPKG_TOOL_SHA="${VCPKG_GLIBC_ARM64_SHA}" ;; \
        *) echo "Unsupported TARGETARCH: ${TARGETARCH}" >&2; exit 1 ;; \
    esac \
    && curl --fail --location --retry 5 \
        "${VCPKG_ASSET_PREFIX}/microsoft/vcpkg-tool/releases/download/${VCPKG_TOOL_RELEASE_TAG}/${VCPKG_TOOL_NAME}" \
        --output /opt/vcpkg/vcpkg \
    && echo "${VCPKG_TOOL_SHA}  /opt/vcpkg/vcpkg" | sha512sum --check --strict \
    && chmod +x /opt/vcpkg/vcpkg \
    && /opt/vcpkg/vcpkg version --disable-metrics

WORKDIR /src
ARG VCPKG_GITHUB_ASSET_PREFIX
ARG VCPKG_SQLITE_MIRROR_PREFIX
ENV VCPKG_GITHUB_ASSET_PREFIX=${VCPKG_GITHUB_ASSET_PREFIX} \
    VCPKG_SQLITE_MIRROR_PREFIX=${VCPKG_SQLITE_MIRROR_PREFIX} \
    X_VCPKG_ASSET_SOURCES="x-script,/usr/local/bin/vcpkg-download.sh {url} {dst};x-block-origin"

COPY docker/vcpkg-download.sh /usr/local/bin/vcpkg-download.sh
RUN chmod +x /usr/local/bin/vcpkg-download.sh

COPY vcpkg.json ./
RUN --mount=type=cache,id=clip-worker-vcpkg-downloads,target=/opt/vcpkg/downloads \
    --mount=type=cache,id=clip-worker-vcpkg-archives,target=/root/.cache/vcpkg/archives \
    case "${TARGETARCH}" in \
        amd64) export VCPKG_TARGET_TRIPLET=x64-linux ;; \
        arm64) export VCPKG_TARGET_TRIPLET=arm64-linux ;; \
        *) echo "Unsupported TARGETARCH: ${TARGETARCH}" >&2; exit 1 ;; \
    esac \
    && /opt/vcpkg/vcpkg install --triplet "${VCPKG_TARGET_TRIPLET}"

COPY CMakeLists.txt CMakePresets.json ./
COPY include include
COPY src src
COPY tests tests

RUN case "${TARGETARCH}" in \
        amd64) export VCPKG_TARGET_TRIPLET=x64-linux ;; \
        arm64) export VCPKG_TARGET_TRIPLET=arm64-linux ;; \
        *) echo "Unsupported TARGETARCH: ${TARGETARCH}" >&2; exit 1 ;; \
    esac \
    && cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_TARGET_TRIPLET="${VCPKG_TARGET_TRIPLET}" \
        -DVCPKG_INSTALLED_DIR=/src/vcpkg_installed \
        -DCLIP_WORKER_BUILD_TESTS=ON \
    && cmake --build build --parallel \
    && ctest --test-dir build --output-on-failure \
    && cmake --install build --prefix /opt/clip-worker \
    && mkdir -p /opt/clip-worker/lib \
    && mkdir -p /opt/clip-worker/share/proj \
    && cp -a "/src/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/share/proj/." \
        /opt/clip-worker/share/proj/

FROM ${BASE_IMAGE} AS runtime

RUN apt-get update \
    && apt-get install --yes --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --create-home clip-worker

COPY --from=build /opt/clip-worker/bin/3d-tiles-clip-worker /usr/local/bin/3d-tiles-clip-worker
COPY --from=build /opt/clip-worker/lib/ /usr/local/lib/
COPY --from=build /opt/clip-worker/share/proj/ /usr/local/share/proj/

ENV LD_LIBRARY_PATH=/usr/local/lib \
    PROJ_DATA=/usr/local/share/proj

USER 10001
ENTRYPOINT ["/usr/local/bin/3d-tiles-clip-worker"]
CMD ["run"]
