#!/usr/bin/env python3
"""Synthetic scaling benchmark used to select bounded Worker defaults."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import platform
import statistics
import time
import tracemalloc
import zipfile
from pathlib import Path, PurePosixPath
from typing import Callable, Iterable, TypeVar

from PIL import Image


BENCHMARK_VERSION = "resource-limit-scaling-v1"
ZIP_ENTRY_PAYLOAD_BYTES = 128
URI_DEPTH_LIMIT_CANDIDATE = 32
RGBA_CHANNEL_COUNT = 4
T = TypeVar("T")
Point = tuple[float, float]


def measure(callback: Callable[[], T], repeats: int = 3) -> dict[str, object]:
    durations: list[float] = []
    peaks: list[int] = []
    result: object = None
    for _ in range(repeats):
        tracemalloc.start()
        started = time.perf_counter_ns()
        result = callback()
        durations.append((time.perf_counter_ns() - started) / 1_000_000)
        _, peak = tracemalloc.get_traced_memory()
        tracemalloc.stop()
        peaks.append(peak)
    return {
        "medianMs": round(statistics.median(durations), 3),
        "peakPythonBytes": max(peaks),
        "result": result,
    }


def deterministic_zip(entry_count: int) -> bytes:
    output = io.BytesIO()
    with zipfile.ZipFile(
        output, mode="w", compression=zipfile.ZIP_DEFLATED, compresslevel=6
    ) as archive:
        for index in range(entry_count):
            info = zipfile.ZipInfo(
                f"tiles/{index // 1000:03d}/content-{index:07d}.bin",
                date_time=(2020, 1, 1, 0, 0, 0),
            )
            info.compress_type = zipfile.ZIP_DEFLATED
            digest = hashlib.sha256(str(index).encode("ascii")).digest()
            archive.writestr(info, digest * (ZIP_ENTRY_PAYLOAD_BYTES // len(digest)))
    return output.getvalue()


def inspect_zip(payload: bytes) -> dict[str, int]:
    expanded_bytes = 0
    maximum_depth = 0
    normalized_names: set[str] = set()
    with zipfile.ZipFile(io.BytesIO(payload), mode="r") as archive:
        for entry in archive.infolist():
            path = PurePosixPath(entry.filename)
            if path.is_absolute() or ".." in path.parts:
                raise RuntimeError("unsafe generated archive path")
            normalized = path.as_posix().casefold()
            if normalized in normalized_names:
                raise RuntimeError("duplicate generated archive path")
            normalized_names.add(normalized)
            expanded_bytes += entry.file_size
            maximum_depth = max(maximum_depth, len(path.parts))
    return {
        "entries": len(normalized_names),
        "expandedBytes": expanded_bytes,
        "maximumPathDepth": maximum_depth,
    }


def hash_resource_closure(resource_count: int) -> str:
    records: list[tuple[str, bytes]] = []
    for index in range(resource_count):
        uri = f"documents/{index // 1000:03d}/resource-{index:07d}.bin"
        records.append((uri, hashlib.sha256(uri.encode("ascii")).digest()))
    closure = hashlib.sha256()
    for uri, digest in sorted(records):
        closure.update(len(uri).to_bytes(4, "little"))
        closure.update(uri.encode("ascii"))
        closure.update(digest)
    return closure.hexdigest()


def clip_edge(
    polygon: list[Point],
    inside: Callable[[Point], bool],
    intersect: Callable[[Point, Point], Point],
) -> list[Point]:
    if not polygon:
        return []
    result: list[Point] = []
    previous = polygon[-1]
    previous_inside = inside(previous)
    for current in polygon:
        current_inside = inside(current)
        if current_inside != previous_inside:
            result.append(intersect(previous, current))
        if current_inside:
            result.append(current)
        previous = current
        previous_inside = current_inside
    return result


def intersection_x(a: Point, b: Point, x: float) -> Point:
    ratio = (x - a[0]) / (b[0] - a[0])
    return (x, a[1] + ratio * (b[1] - a[1]))


def intersection_y(a: Point, b: Point, y: float) -> Point:
    ratio = (y - a[1]) / (b[1] - a[1])
    return (a[0] + ratio * (b[0] - a[0]), y)


def clip_triangle(triangle: Iterable[Point]) -> int:
    polygon = list(triangle)
    polygon = clip_edge(
        polygon, lambda point: point[0] >= 0.0, lambda a, b: intersection_x(a, b, 0.0)
    )
    polygon = clip_edge(
        polygon, lambda point: point[0] <= 1.0, lambda a, b: intersection_x(a, b, 1.0)
    )
    polygon = clip_edge(
        polygon, lambda point: point[1] >= 0.0, lambda a, b: intersection_y(a, b, 0.0)
    )
    polygon = clip_edge(
        polygon, lambda point: point[1] <= 1.0, lambda a, b: intersection_y(a, b, 1.0)
    )
    return max(len(polygon) - 2, 0)


def clip_triangles(triangle_count: int) -> int:
    output_count = 0
    for index in range(triangle_count):
        offset = (index % 101) / 100.0
        output_count += clip_triangle(
            ((offset - 0.2, -0.1), (offset + 0.8, 0.4), (offset, 1.1))
        )
    return output_count


def encode_png(dimension: int) -> int:
    pixels = bytearray(dimension * dimension * RGBA_CHANNEL_COUNT)
    radius_squared = (dimension * 0.3) ** 2
    for y in range(dimension):
        for x in range(dimension):
            offset = (y * dimension + x) * RGBA_CHANNEL_COUNT
            inside = (x - dimension / 2) ** 2 + (y - dimension / 2) ** 2 <= radius_squared
            pixels[offset : offset + 4] = bytes(
                ((x * 13) & 0xFF if inside else 0,
                 (y * 7) & 0xFF if inside else 0,
                 160 if inside else 0,
                 255 if inside else 0)
            )
    image = Image.frombytes("RGBA", (dimension, dimension), bytes(pixels))
    output = io.BytesIO()
    image.save(output, format="PNG", compress_level=9, optimize=False)
    with Image.open(io.BytesIO(output.getvalue())) as decoded:
        if decoded.convert("RGBA").tobytes() != image.tobytes():
            raise RuntimeError("PNG scaling benchmark failed exact RGBA round-trip")
    return len(output.getvalue())


def scaled(measurement: dict[str, object], scale: int, unit: str) -> dict[str, object]:
    return {"scale": scale, "unit": unit, **measurement}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    parser.add_argument("--quick", action="store_true")
    args = parser.parse_args()

    zip_counts = [1_000, 10_000] if args.quick else [1_000, 10_000, 50_000]
    resource_counts = [10_000, 50_000] if args.quick else [10_000, 50_000, 100_000]
    triangle_counts = [10_000, 50_000] if args.quick else [10_000, 100_000, 500_000]
    texture_dimensions = [512, 1024] if args.quick else [512, 1024, 2048]

    inspection: list[dict[str, object]] = []
    for count in zip_counts:
        payload = deterministic_zip(count)
        entry = scaled(measure(lambda: inspect_zip(payload)), count, "entries")
        entry["compressedBytes"] = len(payload)
        inspection.append(entry)

    normalization = [
        scaled(measure(lambda count=count: hash_resource_closure(count)), count, "resources")
        for count in resource_counts
    ]
    clipping = [
        scaled(measure(lambda count=count: clip_triangles(count)), count, "triangles")
        for count in triangle_counts
    ]
    textures = [
        scaled(measure(lambda dimension=dimension: encode_png(dimension)), dimension, "pixelsPerAxis")
        for dimension in texture_dimensions
    ]

    report = {
        "schemaVersion": 1,
        "benchmarkVersion": BENCHMARK_VERSION,
        "environment": {
            "python": platform.python_version(),
            "pillow": Image.__version__,
            "platform": platform.platform(),
            "note": "Python scaling harness; native Worker CI benchmark remains required",
        },
        "candidateUriDepth": URI_DEPTH_LIMIT_CANDIDATE,
        "inspection": inspection,
        "normalization": normalization,
        "clipping": clipping,
        "texture": textures,
    }
    serialized = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(serialized, encoding="utf-8", newline="\n")
    print(serialized, end="")


if __name__ == "__main__":
    main()
