# Deterministic security and geometry fixtures

`fixture-cases.json` declares the required adversarial and boundary cases. The
PowerShell generator writes small, reviewable fixtures under `generated` and a
SHA-256 manifest. Generated outputs are committed because they are independent
of third-party data and are needed by offline tests.

Generate or refresh the declared files:

```powershell
pwsh -File tests/fixtures/generate_fixtures.ps1
```

Verify committed bytes without modifying them:

```powershell
pwsh -File tests/fixtures/generate_fixtures.ps1 -VerifyOnly
```

The fixtures deliberately include invalid and hostile inputs. They must only be
read by bounded parsers in tests; do not open the ZIP with an automatic preview
or pass the traversal URIs to a generic downloader.

The `metadata-leakage` case contains the UTF-8 marker `secret` in the removed
feature row. A safe limited-authorization artifact must retain feature 0 while
making that marker and its unused byte range unrecoverable. The EMPTY completion
fixture intentionally contains no output object identity, digest, or size.
