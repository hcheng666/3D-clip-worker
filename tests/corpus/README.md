# Official 3D Tiles conformance corpus

This directory records the public samples used to prove format detection,
normalization, clipping, metadata isolation, and renderer compatibility. Large
third-party assets are not committed. The manifest pins every source to a full
Git commit and the fetch script materializes only the selected paths.

## Sources and licenses

| Source | Purpose | License handling |
| --- | --- | --- |
| CesiumJS `Specs/Data/Cesium3DTiles` | Legacy and 1.1 content, explicit/implicit hierarchy, external Tilesets, multiple contents, and metadata | Apache-2.0; root `LICENSE.md` is copied with each materialized entry. |
| Cesium 3D Tiles Validator `specs/data` | Valid 3TZ, sparse implicit hierarchy, full metadata, and invalid Feature ID cases | Apache-2.0; root `LICENSE` is copied with each materialized entry. |
| Khronos glTF Sample Assets | GLB/glTF plus Draco, Meshopt, KTX2/BasisU, PNG, JPEG, and WebP variants | Model-specific license and attribution from each model's `LICENSE.md`/`README.md` are copied with the entry. |

The samples remain governed by their upstream licenses. Do not redistribute the
materialized `downloaded` directory without retaining the copied license and
attribution files. The corpus is test data, not a claim that every listed case is
currently safe for limited authorization.

## Validate or fetch

Validate the manifest without network access:

```powershell
pwsh -File tests/corpus/fetch_official_corpus.ps1 -ValidateOnly
```

Fetch the pinned data:

```powershell
pwsh -File tests/corpus/fetch_official_corpus.ps1
```

If a validated partial-clone cache already contains the pinned commit tree but
its promisor blob transport is unavailable, the GitHub-only raw fallback keeps
the same manifest revision and path identities:

```powershell
pwsh -File tests/corpus/materialize_cached_git_tree_raw.ps1 `
  -EntryId cesium-point-cloud,cesium-instanced,cesium-composite
```

The fallback rejects non-GitHub remotes, a mismatched cached origin, absent
pinned commits, unsafe relative paths, existing destinations, and failed raw
responses. It writes the same `payload`, `licenses`, and `SOURCE.json` layout.

Repositories are kept under `tests/corpus/.cache` and selected entries are copied
to `tests/corpus/downloaded/<entry-id>/payload`. Both directories are ignored by
Git. A fetch fails when the remote, pinned revision, selected path, license path,
or required coverage set does not match the manifest.

Validate the Task 6 Mesh entry set, stable outcomes, profile identity, and
required geometry/texture/compression/transform/metadata/empty/determinism/
resource-limit slices without downloading payloads:

```powershell
python tests/corpus/validate_mesh_phase_corpus.py
```

The release gate additionally requires the six pinned payloads and their
`SOURCE.json` revision evidence:

```powershell
python tests/corpus/validate_mesh_phase_corpus.py --require-materialized
```

Algorithmic behavior is exercised by the native generated/differential tests;
the materialized gate prevents those results from being presented as official
corpus coverage when the fixed upstream payloads were not actually present.

Validate the Task 7 PNTS/I3DM/CMPT entry identities, per-family versions,
V3 resource profile, stable outcomes, and native clipping/compaction evidence:

```powershell
python tests/corpus/validate_point_instance_composite_phase_corpus.py
```

The Task 7 release gate additionally requires the three pinned Cesium payloads:

```powershell
python tests/corpus/validate_point_instance_composite_phase_corpus.py --require-materialized
```

This second command is intentionally fail-closed when the pinned Git blobs have
not been downloaded; a manifest-only validation is not reported as official
payload execution.

Validate the Task 8 metadata protocol/profile/matrix identities, fixed official
variant outcomes, stable reasons, and native property-lookup/leakage evidence:

```powershell
python tests/corpus/validate_metadata_privacy_phase_corpus.py
```

The Task 8 release gate additionally requires each pinned payload and a
`RESULTS.metadata-v1.json` produced by actual parser, normalizer, property
lookup, removed-ID, leakage-scan, and determinism execution. Merely downloading
the files does not satisfy this gate:

```powershell
cmake --preset release
cmake --build --preset release --target metadata_privacy_corpus_runner
./build/release/metadata_privacy_corpus_runner
python tests/corpus/validate_metadata_privacy_phase_corpus.py --require-materialized
```

On Windows, invoke the runner as
`./build/release/metadata_privacy_corpus_runner.exe`. The runner reads the
materialized pinned files, executes the supported production parser/normalizer
paths, verifies the fail-closed document/content variants, and atomically
publishes the ignored result files only after every required assertion passes.
The Release CTest suite also registers this runner as
`MetadataPrivacyOfficialCorpus`.

`cesium-tilesets-deterministic-zip` is a derived fixture. The deterministic
fixture generator owns its byte layout and inherits the source entry's license;
the official downloader intentionally does not create archives.
