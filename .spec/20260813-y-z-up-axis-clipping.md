# Verified Y/Z up-axis clipping and X-ready contract

## Context

The control plane now resolves the glTF up axis per mesh-bearing root or external Tileset document.
The worker must consume that verified per-tile decision instead of assuming Z-up. Production support
is deliberately limited to Y and Z; X is represented in the contract so future enablement does not
require another structural rewrite, but X tasks remain rejected until real X-up golden data and a new
algorithm version are approved.

## Implemented behavior

1. `GltfUpAxis` contains X, Y and Z. Parsing accepts Y/Z, recognizes X and returns an explicit
   production-disabled error, and rejects missing, non-string, lowercase or unknown values.
2. World projection composes `tile transform * RTC translation * up-axis transform * node transform`.
   Z is identity, Y maps `(x,y,z)` to `(x,-z,y)`, and the reserved X strategy maps `(x,y,z)` to
   `(-z,y,x)`.
3. Clipping and interpolation continue in the existing projected frame. Generated positions and
   normals remain in the source-local coordinate system; the worker does not bake the up-axis
   transform into output GLB data.
4. Algorithm identity is `v5`. Program, CMake, Compose image and documentation identity are `0.1.4`.
   The old v4 output is immutable and cannot be claimed by v5 workers.

## Tests and evidence

1. Task-contract tests accept Y and Z, reject X as currently unsupported, and reject malformed axes.
2. Synthetic Y and Z fixtures encode the same world geometry in different source-local coordinates.
   Golden clipping asserts equal vertex/triangle counts and texture bytes, then verifies each Y-local
   output position is the inverse Y-up representation of its corresponding Z-local output position.
3. The existing RTC, node transform, geometry, attribute, texture, EMPTY, deterministic-output and
   fail-closed suites remain enabled. The offline Release Docker build ran 63 CTest cases with zero
   failures.
4. The generated local amd64 image is `3d-tiles-clip-worker:0.1.4`, image ID
   `sha256:5d2351db247029f1200c8c123d74fdc8096c134b6f12b3fdfb65562ae1ba9dc6`.

## Deployment boundary

No deployment was performed. SQL/control-plane/gateway and v5 workers must be deployed in the
documented synchronized order. X must not be added to the production capability set without real
X-up registration, index, clip and rendering goldens plus another algorithm-version migration.

## 2026-08-15 bounded Draco and zero-batch extension

### Confirmed input and security boundary

1. The NJ04 limited authorization selects 917 boundary B3DM assets. Read-only inspection found required
   `KHR_draco_mesh_compression` in all 917, including 42 valid zero-batch containers and 875 single-batch
   containers. No multi-batch or Feature/Batch Binary sample was observed.
2. The worker decodes bounded triangular Draco meshes through the typed C++ decoder, expands supported
   attributes through Draco point mappings, and feeds the existing transform, exact clipper, interpolation,
   texture mask, and BufferBuilder pipeline. It never passes compressed boundary geometry through.
3. Output is deterministic uncompressed GLB/B3DM and removes Draco-only declarations and buffer views so
   lossy re-quantization cannot move newly created vertices across the authorization boundary.
4. `BATCH_LENGTH=0` and `1` are accepted only without binary tables. Zero-batch metadata stays empty;
   single-batch JSON metadata is sanitized as before. Multi-batch and binary-table semantics remain closed.

### Real-data compatibility correction

The first development-only v6 run exposed a producer compatibility issue absent from the synthetic
fixture: attribute accessors can retain a mutually consistent pre-compression count while Draco decoding changes
the point topology. The decoder already expands every attribute through `PointIndex -> AttributeValueIndex`,
validates faces, mappings, components, finite values, and named resource limits. The worker now treats that
decoded topology as authoritative only when all mapped attribute accessor counts agree with one another,
and emits one `GLTF_STALE_DRACO_ACCESSOR_COUNT` warning. A v7 follow-up found the same stale-count pattern
on one index accessor whose decoded face topology and point references were valid; index count now follows
the same warning path. Inconsistent attribute counts and invalid decoded topology still fail closed.

Because the v6/v7 development jobs already contain immutable terminal evidence, the corrected contract uses
algorithm identity `v8` and program/image version `0.1.7`. Metadata and Worker defaults must move together;
v5/v6/v7 tasks are preserved and must not be reset or relabeled.

### Verification plan

The implementation must pass all CTest cases, metadata drift/retry tests, Release amd64 and arm64 builds,
strict OpenSpec validation, and diff checks. Development acceptance uses normal v8 reconciliation without
task-state DML and requires all 917 NJ04 assets to reach READY or truthful EMPTY with zero failed and
unsupported assets, followed by an authorized-map loading check.
