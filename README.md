# 3D Tiles Clip Worker

Standalone C++17 worker for strict spatial clipping of 3D Tiles mesh content.

The worker is fail closed: unsupported or invalid content is reported to the control plane and is
never passed through as an authorized clipped asset. Production targets are Linux `amd64` and
Linux `arm64`; Windows is supported as a development build environment only. ARMv7 and other
32-bit ARM targets are not supported.

## Current scope

- Strict B3DM v1 and embedded GLB 2.0 parsing, with bounded four-byte-alignment
  compatibility for deterministic legacy input.
- Bounded normalization of known numeric non-standard sampler `wrapR`; two-dimensional
  `wrapS`/`wrapT` masking rules remain strict.
- EPSG:4490 Polygon/MultiPolygon/Hole projection into a local metric frame.
- Triangle/prism clipping with POSITION, TEXCOORD_0, NORMAL and COLOR_0 interpolation.
- Embedded WebP UV masking, zero RGBA outside the mask, and lossless re-encoding.
- Minimal GLB/B3DM reconstruction with active-reference and extension whitelisting.
- Presigned object download/upload with source ETag, size and SHA-256 checks.
- Long-running claim/heartbeat/complete/fail worker loop with graceful signal handling.
- Synthetic fixtures and unit tests; no production dataset is committed.

The first version accepts uncompressed TRIANGLES with one B3DM batch feature and embedded buffers.
Draco, Meshopt, KTX2/BasisU, external glTF resources and unknown required extensions fail closed.

## Local build

Install CMake 3.24+, Ninja, a C++17 compiler, and vcpkg. Set `VCPKG_ROOT`, then run:

```powershell
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

## Worker configuration

The container starts the `run` command by default. Required deployment variables are:

| Variable | Purpose |
| --- | --- |
| `CLIP_WORKER_CONTROL_PLANE_URL` | Base URL of the metadata control plane |
| `CLIP_WORKER_ID` | Unique worker/pod identifier; falls back to `HOSTNAME` |

Optional variables are `CLIP_WORKER_AUTHORIZATION_HEADER`,
`CLIP_WORKER_ALGORITHM_VERSION` (default `v4`), `CLIP_WORKER_MAX_INPUT_BYTES`,
`CLIP_WORKER_MAX_OUTPUT_BYTES`, `CLIP_WORKER_POLL_INTERVAL_SECONDS`,
`CLIP_WORKER_HEARTBEAT_INTERVAL_SECONDS`, and the
`CLIP_WORKER_API_*_TIMEOUT_SECONDS` / `CLIP_WORKER_TRANSFER_*_TIMEOUT_SECONDS` settings.
`CLIP_WORKER_LOG_LEVEL` accepts `DEBUG`, `INFO`, `WARN`, or `ERROR` and defaults to `INFO`.
The authorization value is a complete HTTP header such as `Authorization: Bearer ...`; never put it
in an image layer or log output.

## Structured logs

The worker writes one JSON object per event with a UTC millisecond `timestamp`, `level`, stable
`event` name and human-readable `message`. Task events also include `workerId` and `assetId`.
Successful tasks end with exactly one `task.succeeded` event containing `READY` or `EMPTY`, total
duration, object sizes, and clipping statistics. Failed tasks end with exactly one `task.failed`
event containing the failed stage, error type and code, retryability, duration, and whether the
failure was reported to the control plane.

The default `INFO` level records task claims, completed download/clip/upload phases, final results,
and all warnings and errors. `DEBUG` additionally records empty polls, successful heartbeats, and
phase starts. Logs never include authorization headers, lease tokens, presigned URLs, claim or HTTP
response bodies, scope WKB, output hashes, or ETags.

Legacy B3DM whose header-declared embedded GLB is four-byte aligned but not eight-byte aligned is
accepted only when the GLB length, chunks and content pass all existing strict validation. Such an
asset emits one `source.compatibility_warning` WARN event with compatibility code
`B3DM_NONCONFORMANT_ALIGNMENT` and numeric alignment diagnostics. Non-empty output is always
rebuilt with an eight-byte-aligned GLB start and tile length.

Producer-added glTF sampler `wrapR` is accepted only when it is an unsigned known WebGL wrap
enum. The field has no meaning for the Worker's two-dimensional `TEXCOORD_0` mask, so it is
validated, reported once as `GLTF_NONSTANDARD_SAMPLER_WRAP_R`, ignored during masking, and removed
from normalized output. Invalid `wrapR` values and unsupported `wrapS`/`wrapT` modes still fail
closed.

Follow application events directly, or add Docker's receive timestamp for comparison:

```powershell
docker compose logs --follow clip-worker
docker compose logs --follow --timestamps clip-worker
```

## Docker

Build and test the current Docker platform:

```powershell
docker build --tag 3d-tiles-clip-worker:dev .
```

Build and push both production architectures:

```powershell
docker buildx build --platform linux/amd64,linux/arm64 --tag <registry>/3d-tiles-clip-worker:<version> --push .
```

The multi-platform build uses the explicit vcpkg triplets `x64-linux` and `arm64-linux`, runs the
complete test suite in each build stage, and copies vcpkg runtime libraries plus PROJ data into the
matching final image. Every control-plane request includes the required internal header
`from-source: inner`.

## Offline native build

The offline build separates stable dependencies from frequently changing Worker source code:

- `build-base` contains the compiler, CMake, Ninja, the pinned vcpkg toolchain and all development
  dependencies from `vcpkg.json`.
- `runtime-base` contains only CA certificates, system runtime libraries, PROJ data and UID 10001.
- `Dockerfile.offline` compiles the current source and runs all CTest cases, then copies only the
  installed Worker files into `runtime-base`.

The dependency bundle and Worker project directory are separate deliverables. Generate one bundle
on a connected native amd64 Linux host and another on a connected native arm64 Linux host. QEMU,
binfmt, ARMv7 and other 32-bit targets are intentionally unsupported.

Native dependency-bundle preparation and offline application builds support Docker's classic
builder and do not require the buildx plugin. Cross-architecture or combined multi-platform
registry builds still require buildx, as shown in the Docker section above.

### Prepare a dependency bundle online

Run from a connected Linux checkout. The output directory must be empty and should be outside the
Docker build context:

```sh
chmod +x docker/offline/*.sh
./docker/offline/prepare-bundle.sh /srv/offline-bundles/clip-worker-deps
```

Do not prefix this command with `DOCKER_BUILDKIT=1` on a host without buildx. If that variable is
already exported, run `unset DOCKER_BUILDKIT`; the script will then use the supported classic
builder path and pass the detected native `amd64` or `arm64` architecture explicitly.

If Docker Hub or GitHub is not reachable from a China-based build host, select the explicit
domestic mirror profile:

```sh
./docker/offline/prepare-bundle.sh --mirror-profile cn \
  /srv/offline-bundles/clip-worker-arm64-r1
```

The `cn` profile uses the pinned multi-architecture Ubuntu 24.04 OCI index from DaoCloud, routes
the pinned vcpkg repository, tool and GitHub assets through `gh-proxy.com`, and uses Aliyun mirrors
for SQLite and Ubuntu packages. It selects `http://mirrors.aliyun.com/ubuntu` on amd64 and
`http://mirrors.aliyun.com/ubuntu-ports` on arm64. HTTP is required for the initial package index
because the minimal Ubuntu base does not yet contain CA certificates; Ubuntu `InRelease`
signatures and package hashes remain mandatory. The profile has been checked for a
`linux/arm64/v8` Ubuntu manifest. It does not disable any vcpkg tool, source asset, APT signature,
manifest or bundle checksum validation. The original single-argument command continues to use
official sources.

The script builds architecture-suffixed `build-base` and `runtime-base` images, validates their
platform and labels, exercises their required files with networking disabled, and writes:

```text
clip-worker-deps/
  build-base-<deps-version>-<arch>.tar
  runtime-base-<deps-version>-<arch>.tar
  manifest.properties
  SHA256SUMS
  README.md
```

Classic-builder compatibility means dependency downloads are not stored in BuildKit cache mounts.
Because bundle preparation also uses `--no-cache`, a failed preparation attempt may download the
vcpkg dependencies again when retried.

Build mirrors can be supplied through the existing `CLIP_WORKER_BASE_IMAGE` and
`CLIP_WORKER_VCPKG_*` environment variables. `CLIP_WORKER_APT_MIRROR` overrides the architecture
specific package mirror used by `Dockerfile.base`. Never put credentials in those values because
image build arguments, bundle manifests and layers are not secret stores. Explicit environment
values override the chosen mirror profile, which allows an approved internal Registry or proxy to
replace any public mirror. There is no automatic public-mirror fallback.

### Load and build without a network

Copy the matching bundle and the Worker project directory to the offline Linux host. From the
project root, verify every bundle file, verify the native architecture and manifest, and load the
two base images:

```sh
./docker/offline/load-bundle.sh /media/offline/clip-worker-deps
```

Build and smoke-test a fresh application image. This command always disables network access,
remote pulls and Docker build-step cache:

```sh
./docker/offline/build.sh 3d-tiles-clip-worker:0.1.3
```

The build fails before compilation if the current `vcpkg.json` SHA-256 differs from the manifest
embedded in `build-base`. Changes under `include/`, `src/`, `tests/` or the CMake project reuse the
same base images. Changes to `vcpkg.json`, Ubuntu, vcpkg, the compiler baseline or dependency build
options require a new dependency version and new native bundles for both architectures.

Version `0.1.3` does not change `vcpkg.json` or the dependency toolchain, so the existing matching
amd64/arm64 offline bundles remain valid. Copy the updated project source and build only the final
application image. Deploy metadata property `auth.three-d-clip.algorithm-version=v4` and Worker
environment `CLIP_WORKER_ALGORITHM_VERSION=v4` together. Development-only `v1`-`v3` clip
derivatives may be cleaned after exact target inventory; the control plane creates a new `v4`
preparation/job through algorithm-drift reconciliation.

To use Docker Compose for the offline application build, set the architecture-matching
`CLIP_WORKER_BUILD_BASE_IMAGE` and `CLIP_WORKER_RUNTIME_BASE_IMAGE` values in `.env`, then run:

```sh
docker compose -f compose.yaml -f compose.offline.yaml config
docker compose -f compose.yaml -f compose.offline.yaml build --no-cache
docker compose up --detach --no-build
```

The first two commands use `Dockerfile.offline`, `network: none` and `pull: false`. The final
runtime command uses the normal Compose service and its existing read-only root filesystem,
capability, log rotation and stop-grace-period settings.

## Docker Compose

The Compose service has no inbound ports. It only calls the metadata control plane and the MinIO
presigned URLs returned by that control plane. Create the local deployment configuration and
validate all required variables before starting:

```powershell
Copy-Item .env.example .env
docker compose config
```

For a native build on the current amd64 or arm64 host, run:

```powershell
docker compose build
docker compose up --detach
```

The Dockerfile runs all CTest cases during the build. Compose intentionally does not set
`platform`, so a native build uses the host architecture and a registry image uses the matching
entry from its multi-platform manifest.

For a production image that has already been pushed to the registry, set `CLIP_WORKER_IMAGE` in
`.env`, then deploy without rebuilding:

```powershell
docker compose pull
docker compose up --detach --no-build
```

Use the following commands for routine operations:

```powershell
docker compose ps
docker compose logs --follow clip-worker
docker compose up --detach --no-build --scale clip-worker=3
docker compose stop
docker compose down
```

Leave `CLIP_WORKER_ID` empty when scaling so each replica uses its unique Docker `HOSTNAME`.
The container runs as UID 10001 with a read-only root filesystem, no Linux capabilities and no
new privileges. The default ten-minute stop grace period allows an in-flight task to finish after
SIGTERM; tune it to the production maximum processing time. Do not store authorization headers in
the image or commit the local `.env` file.

### Replica rollout and rollback

Keep the application default at one replica. For a fixed new job, fixed scope and equal total CPU
and memory accounting, run one, two and four replicas in order:

```powershell
docker compose up --detach --no-build --scale clip-worker=1
docker compose up --detach --no-build --scale clip-worker=2
docker compose up --detach --no-build --scale clip-worker=4
```

Do not set a shared `CLIP_WORKER_ID` while scaling. Before moving to the next level, let the fixed
job reach a truthful terminal state and record completed assets per minute, download/clip/upload
P95 and P99, retry/failure counts, per-replica peak memory, total CPU, MinIO throughput and errors,
metadata claim/heartbeat/complete latency, and PostgreSQL lock waits. Stop increasing replicas when
throughput no longer improves materially or any correctness, failure, memory, object-store or
control-plane metric regresses. Account for memory as peak memory per replica multiplied by the
replica count; CPU count alone is not a safe sizing rule.

To roll back capacity without changing job data, return to the last safe count, usually one:

```powershell
docker compose up --detach --no-build --scale clip-worker=1
```

Compose sends `SIGTERM` and honors `CLIP_WORKER_STOP_GRACE_PERIOD`. An in-flight asset may complete
during that interval; if a process cannot finish, the existing lease expiry path makes the asset
claimable again. Do not reset jobs or READY assets during replica rollback. Metadata and every
running Worker must advertise the same algorithm version, currently `v4`.
