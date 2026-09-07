#!/usr/bin/env python3
"""Deterministic PNG versus exact lossless WebP policy benchmark."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import platform
import statistics
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from PIL import Image, features


DEFAULT_IMAGE_DIMENSION = 512
DEFAULT_ITERATIONS = 7
RGBA_CHANNEL_COUNT = 4


@dataclass(frozen=True)
class Codec:
    name: str
    mime_type: str
    extension_required: bool
    encode: Callable[[Image.Image], bytes]


def make_base_pixels(width: int, height: int, high_entropy: bool) -> bytearray:
    pixels = bytearray(width * height * RGBA_CHANNEL_COUNT)
    state = 0x6D2B79F5
    for y in range(height):
        for x in range(width):
            offset = (y * width + x) * RGBA_CHANNEL_COUNT
            if high_entropy:
                state = (1_664_525 * state + 1_013_904_223) & 0xFFFFFFFF
                red = state & 0xFF
                green = (state >> 8) & 0xFF
                blue = (state >> 16) & 0xFF
            else:
                red = (x * 255) // max(width - 1, 1)
                green = (y * 255) // max(height - 1, 1)
                blue = ((x // 16) * 29 + (y // 16) * 17) & 0xFF
            pixels[offset : offset + 4] = bytes((red, green, blue, 255))
    return pixels


def apply_alpha_case(
    pixels: bytearray, width: int, height: int, case_name: str
) -> None:
    if case_name.startswith("opaque"):
        return

    center_x = width / 2.0
    center_y = height / 2.0
    authorized_radius = min(width, height) * 0.28
    feather_width = 3.0
    for y in range(height):
        for x in range(width):
            offset = (y * width + x) * RGBA_CHANNEL_COUNT
            distance = ((x - center_x) ** 2 + (y - center_y) ** 2) ** 0.5
            if case_name == "sparse-transparent-mask":
                alpha = 255 if distance <= authorized_radius else 0
            elif distance <= authorized_radius - feather_width:
                alpha = 255
            elif distance >= authorized_radius + feather_width:
                alpha = 0
            else:
                alpha = round(
                    255
                    * (authorized_radius + feather_width - distance)
                    / (2 * feather_width)
                )

            pixels[offset + 3] = alpha
            if alpha == 0:
                # Canonical security invariant: hidden RGB outside the mask is zero.
                pixels[offset : offset + 3] = b"\x00\x00\x00"


def make_case(case_name: str, dimension: int) -> Image.Image:
    high_entropy = case_name == "opaque-high-entropy"
    pixels = make_base_pixels(dimension, dimension, high_entropy)
    apply_alpha_case(pixels, dimension, dimension, case_name)
    return Image.frombytes("RGBA", (dimension, dimension), bytes(pixels))


def encode_png(image: Image.Image) -> bytes:
    output = io.BytesIO()
    image.save(output, format="PNG", compress_level=9, optimize=False)
    return output.getvalue()


def encode_webp(image: Image.Image) -> bytes:
    output = io.BytesIO()
    image.save(
        output,
        format="WEBP",
        lossless=True,
        quality=100,
        method=6,
        exact=True,
    )
    return output.getvalue()


def benchmark_codec(
    codec: Codec, image: Image.Image, iterations: int
) -> dict[str, object]:
    expected_pixels = image.tobytes()
    encoded_values: list[bytes] = []
    encode_times_ms: list[float] = []
    decode_times_ms: list[float] = []

    for _ in range(iterations):
        encode_started = time.perf_counter_ns()
        encoded = codec.encode(image)
        encode_times_ms.append((time.perf_counter_ns() - encode_started) / 1_000_000)
        encoded_values.append(encoded)

        decode_started = time.perf_counter_ns()
        with Image.open(io.BytesIO(encoded)) as decoded:
            actual_pixels = decoded.convert("RGBA").tobytes()
        decode_times_ms.append((time.perf_counter_ns() - decode_started) / 1_000_000)
        if actual_pixels != expected_pixels:
            raise RuntimeError(f"{codec.name} failed exact RGBA round-trip")

    first = encoded_values[0]
    if any(candidate != first for candidate in encoded_values[1:]):
        raise RuntimeError(f"{codec.name} output is not byte deterministic")

    return {
        "format": codec.name,
        "mimeType": codec.mime_type,
        "requiresGltfExtension": codec.extension_required,
        "encodedBytes": len(first),
        "sha256": hashlib.sha256(first).hexdigest(),
        "deterministic": True,
        "exactRgbaRoundTrip": True,
        "medianEncodeMs": round(statistics.median(encode_times_ms), 3),
        "medianDecodeMs": round(statistics.median(decode_times_ms), 3),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dimension", type=int, default=DEFAULT_IMAGE_DIMENSION)
    parser.add_argument("--iterations", type=int, default=DEFAULT_ITERATIONS)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.dimension <= 0 or args.iterations < 2:
        parser.error("dimension must be positive and iterations must be at least 2")

    codecs = [
        Codec("PNG", "image/png", False, encode_png),
        Codec("WEBP_LOSSLESS_EXACT", "image/webp", True, encode_webp),
    ]
    case_names = [
        "opaque-smooth",
        "opaque-high-entropy",
        "sparse-transparent-mask",
        "feathered-alpha-boundary",
    ]
    cases: list[dict[str, object]] = []
    for case_name in case_names:
        image = make_case(case_name, args.dimension)
        results = [benchmark_codec(codec, image, args.iterations) for codec in codecs]
        png_bytes = int(results[0]["encodedBytes"])
        webp_bytes = int(results[1]["encodedBytes"])
        cases.append(
            {
                "id": case_name,
                "width": args.dimension,
                "height": args.dimension,
                "rawRgbaBytes": len(image.tobytes()),
                "results": results,
                "webpToPngSizeRatio": round(webp_bytes / png_bytes, 4),
            }
        )

    report = {
        "schemaVersion": 1,
        "benchmarkVersion": "texture-codec-policy-v1",
        "iterations": args.iterations,
        "environment": {
            "python": platform.python_version(),
            "pillow": Image.__version__,
            "libwebp": features.version("webp"),
            "zlib": features.version("zlib"),
            "platform": platform.platform(),
        },
        "encoderSettings": {
            "png": {"compressLevel": 9, "optimize": False},
            "webp": {"lossless": True, "quality": 100, "method": 6, "exact": True},
        },
        "cases": cases,
    }
    serialized = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(serialized, encoding="utf-8", newline="\n")
    print(serialized, end="")


if __name__ == "__main__":
    main()
