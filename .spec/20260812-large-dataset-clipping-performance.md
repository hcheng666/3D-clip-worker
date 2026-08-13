# 2026-08-12 Large-dataset clipping performance implementation

## Context

The worker previously compared every projected model triangle with every authorization-scope
triangle. The exact clipper is correct and fail closed, but its geometry cost grows approximately
with the product of model-triangle and scope-triangle counts. The control-plane Spec is maintained
at `D:/code/api-platform/.spec/20260811-authorization-triggered-3d-clip-precompute.md`, section 30.

The user confirmed a direct algorithm upgrade to `v4`. Development-only `v1` through `v3`
derivatives may be discarded after exact inventory; source 3D Tiles, indexes, authorization state,
audit state, service storage configuration and unrelated objects are outside cleanup scope.

## Implemented behavior

1. `AuthorizationTriangleIndex` owns the triangulated authorization scope and builds an immutable,
   deterministic AABB hierarchy once per claimed asset.
2. A scope-triangle bound conservatively encloses the three half-planes expanded by the exact
   clipper's `kPlaneEpsilonMeters`. Bounds are expanded outward with `nextafter`; degenerate or
   numerically unstable input uses infinite bounds and therefore cannot cause a false negative.
3. Each model triangle queries the hierarchy by its inclusive projected AABB. Candidate indices
   are sorted back into original triangulation order before the unchanged exact `TriangleClipper`
   runs. The index never grants authorization and never bypasses exact clipping.
4. Query index and triangle buffers are reused across all model triangles in one asset, avoiding a
   per-triangle temporary-vector allocation.
5. Geometry clipping, attribute interpolation, minimum-area removal, UV texture masking, lossless
   WebP encoding, post-encode decode verification, B3DM reconstruction and output validation remain
   unchanged and fail closed.
6. Worker runtime, Compose and `.env.example` default to algorithm `v4`. Program, CMake, README and
   image version are `0.1.3`. The default replica count remains one.

## Replica operation

Compose deployments may benchmark `--scale clip-worker=1`, then `2`, then `4` while
`CLIP_WORKER_ID` is blank so Docker's unique hostname becomes the worker identity. Scale-up stops
when throughput no longer improves materially or CPU, total memory, MinIO throughput/errors,
metadata latency, PostgreSQL lock waits, retries or failures regress. Rollback returns to the last
safe count and relies on graceful stop plus existing lease recovery; jobs and READY assets are not
reset. Detailed commands and guardrails are in `README.md` under "Replica rollout and rollback".

## Correctness tests

The exhaustive clipper remains available as the test oracle. Added tests cover:

1. 1,000 deterministic random model triangles against a 600-triangle scope, comparing fragment
   count, order, projected coordinates, height, local position and attributes.
2. Real WKB triangulation for Polygon with a hole and MultiPolygon, each with deterministic random
   model triangles compared to exhaustive clipping.
3. Plane-tolerance contact, boundary touching, disjoint and tiny triangles.
4. Original authorization-triangle order and candidate reduction for a localized triangle in a
   20,000-triangle dense scope.
5. Byte-for-byte deterministic repeated B3DM output for identical `v4` input.

## Verification result

1. The amd64 dependency bundle SHA-256 values matched `SHA256SUMS` before loading its build and
   runtime base images.
2. `Dockerfile.offline` built with `--network none --pull=false --no-cache` and ran all tests in a
   Release build. Final result: 61 CTest cases passed, zero failed.
3. The generated `3d-tiles-clip-worker:0.1.3` reports version `0.1.3`, runs as UID `10001`, retains
   the runtime PROJ database and contains no build toolchain.
4. Representative production throughput, peak memory and one/two/four-replica results remain an
   environment validation item because no representative large dataset and resource budget were
   supplied. Unit-test timings are not presented as production performance evidence.

## Rollback

The control-plane and every worker must advertise the same algorithm identity. Rolling the worker
back therefore requires rolling metadata configuration back in the same deployment window. Reduce
replicas without data migration; do not relabel `v4` output as another algorithm. A failed indexed
result remains PREPARING/FAILED and must never fall back to original boundary content.
