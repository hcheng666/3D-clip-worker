# Normalizer V1 canonical fixtures

`generate_fixtures.ps1` creates the minimal deterministic Task-5 canonical GLB
fixtures for `MESH_GLTF2`, `POINT_GLTF2`, and `INSTANCE_GLTF2`, plus fail-closed
external-URI, unknown-required-extension, and malformed-length cases.

Regenerate with:

```powershell
pwsh ./tests/fixtures/normalization/generate_fixtures.ps1
```

Verify committed bytes without rewriting them with `-VerifyOnly`.
