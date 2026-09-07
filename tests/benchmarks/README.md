# Texture codec policy benchmark

Run with the workspace Python runtime, which includes Pillow with PNG and WebP support:

```powershell
& 'C:\Users\36524\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' `
  tests/benchmarks/texture_codec_benchmark.py `
  --output tests/benchmarks/results/texture-codec-baseline.json
```

The generated pixels are deterministic and cover opaque smooth data, opaque high-entropy
data, a sparse authorization mask, and a feathered alpha boundary. Every encoder run must
be byte-identical and decode to the exact original RGBA bytes. Transparent pixels always
have zero RGB so hidden source color cannot survive outside the authorization mask.

Timing is informational and machine-dependent. Size, digest, deterministic encoding, and
exact pixel round-trip are the policy signals. The committed baseline is not a substitute
for rerunning the same corpus against the pinned Worker libpng/libwebp builds in CI.

Run the resource-limit scaling harness:

```powershell
& 'C:\Users\36524\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' `
  tests/benchmarks/resource_limit_benchmark.py `
  --output tests/benchmarks/results/resource-limit-baseline.json
```

It measures bounded ZIP central-directory inspection, canonical resource-closure hashing,
triangle clipping growth, and exact PNG encode/decode scaling. The approved first-phase
defaults are published separately in `config/resource-limit-profiles-v1.json`; benchmark
results remain evidence and do not become implicit runtime defaults.

Validate the named profile, cross-field memory relationships, and committed evidence:

```powershell
& 'C:\Users\36524\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' `
  tests/benchmarks/validate_resource_limit_profile.py
```

`STANDARD_4CPU_8GIB_SINGLE_TASK_V1` assumes 4 vCPU, 8 GiB, and one active task. Native
Worker and official-corpus RSS/duration measurements are still required before each new
decoder or content capability is enabled. Increasing a limit requires a new versioned
profile and updated evidence; the legacy B3DM input/output limits remain unchanged here.

Validate the separate hash-closed Task 6 mesh profile:

```powershell
& 'C:\Users\36524\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' `
  tests/benchmarks/validate_mesh_resource_profile.py
```

The V2 validator checks the canonical PNG and `normalization-v2` policy plus cross-field
decoder, geometry, texture, metadata, authorization, output, and resident-memory limits.
It does not change the V1 Inspector/legacy profile identity.

Task 7 point, instance, composite, and metadata limits are independently pinned
in `config/resource-limit-profiles-v3.json`. Validate their exact bytes,
per-family version policy, fail-closed behavior, and cross-limit invariants with:

```powershell
python tests/benchmarks/validate_broad_resource_profile.py
```

Task 8 metadata-safe limits are independently pinned in
`config/resource-limit-profiles-v4.json`. Validate the exact profile bytes,
metadata family identities, fail-closed policy, hierarchy/mapping budgets, and
cross-limit invariants with:

```powershell
python tests/benchmarks/validate_metadata_resource_profile.py
```
