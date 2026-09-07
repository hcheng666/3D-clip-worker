# Canonical clipped texture policy v1

## Decision

Emit **PNG (`image/png`)** as the first-phase canonical texture for any texture that is
decoded, authorization-masked, or otherwise rebuilt by the normalization pipeline.

Keep exact lossless WebP as an opt-in renderer capability after corpus verification. Do not
emit WebP-only content on the general compatibility route. Existing legacy B3DM/WebP output
remains unchanged until the versioned canonical mesh path replaces it; this benchmark does
not silently change current production bytes.

## Rationale

- PNG is a core glTF 2.0 image type and does not need a texture extension.
- WebP-only glTF requires `EXT_texture_webp` in both `extensionsUsed` and
  `extensionsRequired`; a client without the extension cannot use that texture unless a PNG
  or JPEG fallback is also emitted.
- Emitting both WebP and PNG defeats much of the storage saving and expands the resource
  closure, cache surface, validator work, and metadata that authorization output must prove
  safe.
- PNG and WebP both passed deterministic byte output and exact RGBA round-trip for all four
  generated alpha/entropy cases. PNG therefore does not weaken the zero-outside-mask privacy
  invariant.
- WebP was smaller in every generated case, at 10.55% to 63.06% of PNG size. With the exact
  lossless method-6 settings matching the existing Worker, median encoding was about 1.1 to
  2.8 seconds for a 512x512 image, versus about 8 to 293 ms for PNG in this policy harness.
  Timing is machine-dependent, but the gap is large enough to affect bounded task duration.

## Required encoder contract

- Decode every supported source texture to bounded RGBA.
- Set all four channels to zero outside the authorized UV mask. Fully transparent pixels
  must not retain source RGB.
- Encode PNG with pinned libpng/zlib versions and explicit deterministic compression/filter
  settings. Do not write timestamps, ICC, Exif, textual, or other source metadata chunks.
- Decode the exact emitted PNG and verify dimensions, exact RGBA values, and zero pixels
  outside the mask before publish.
- Validate the final glTF/GLB and declare `image/png`; remove obsolete WebP/KTX2 extension
  declarations and unreachable source image payloads.

## Optional WebP route

An exact lossless WebP route may be enabled only when all of the following are true:

- The dataset and selected renderer explicitly advertise verified `EXT_texture_webp` support.
- The encoder uses `lossless=1`, `quality=100`, `method=6`, and `exact=1` with pinned libwebp.
- Post-encode decode verification proves exact RGBA and zero-outside-mask behavior.
- The output identity includes codec policy and encoder version so PNG/WebP artifacts cannot
  collide in caches.

## Baseline

The machine-readable result is `results/texture-codec-baseline.json`. It was produced with
Pillow 12.3.0, libwebp 1.6.0, and zlib-ng 1.3.1 over seven iterations per case. CI must rerun
the same fixtures against the pinned Worker libpng/libwebp implementations before task 6.6
enables the production encoder.
