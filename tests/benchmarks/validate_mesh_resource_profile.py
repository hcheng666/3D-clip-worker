#!/usr/bin/env python3
"""Validate the hash-closed Task 6 mesh resource profile."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
PROFILE_PATH = PROJECT_ROOT / "config" / "resource-limit-profiles-v2.json"
EXPECTED_SCHEMA_VERSION = 2
EXPECTED_PROFILE_ID = "STANDARD_4CPU_8GIB_SINGLE_TASK_V2"
EXPECTED_PROFILE_SHA256 = (
    "bb326727f6b29a6cdd3532d85e2043c3c0ff212056b837d3d4a2eca7df3d349c"
)
EXPECTED_NORMALIZATION_VERSION = "normalization-v2"
EXPECTED_CANONICAL_CONTRACT_VERSION = "canonical-gltf2-v1"
RGBA_CHANNEL_COUNT = 4


def load_json(path: Path) -> dict[str, object]:
    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise AssertionError(f"{path} must contain a JSON object")
    return value


def require_positive_integers(values: dict[str, object], section: str) -> None:
    for name, value in values.items():
        if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
            raise AssertionError(f"{section}.{name} must be a positive integer")


def main() -> None:
    digest = hashlib.sha256(PROFILE_PATH.read_bytes()).hexdigest()
    if digest != EXPECTED_PROFILE_SHA256:
        raise AssertionError(f"mesh resource profile hash drifted: {digest}")

    registry = load_json(PROFILE_PATH)
    if registry.get("schemaVersion") != EXPECTED_SCHEMA_VERSION:
        raise AssertionError("unexpected mesh resource profile schema version")
    if registry.get("defaultProfile") != EXPECTED_PROFILE_ID:
        raise AssertionError("the Task 6 mesh profile must remain the default")

    profiles = registry.get("profiles")
    if not isinstance(profiles, list) or len(profiles) != 1:
        raise AssertionError("the V2 registry must contain exactly one profile")
    profile = profiles[0]
    if not isinstance(profile, dict) or profile.get("id") != EXPECTED_PROFILE_ID:
        raise AssertionError("the approved Task 6 mesh profile is missing")

    numeric_sections = (
        "deployment",
        "resourceClosure",
        "gltf",
        "geometry",
        "texture",
        "metadata",
        "authorization",
        "decoder",
        "deadlineSeconds",
    )
    for section_name in numeric_sections:
        section = profile.get(section_name)
        if not isinstance(section, dict):
            raise AssertionError(f"{section_name} must be an object")
        require_positive_integers(section, section_name)

    deployment = profile["deployment"]
    closure = profile["resourceClosure"]
    gltf = profile["gltf"]
    geometry = profile["geometry"]
    texture = profile["texture"]
    metadata = profile["metadata"]
    authorization = profile["authorization"]
    decoder = profile["decoder"]

    if deployment["maximumConcurrentTasks"] != 1:
        raise AssertionError("the mesh profile must remain single-task")
    if deployment["maximumTaskResidentBytes"] >= deployment["memoryBytes"]:
        raise AssertionError("the task budget must leave container headroom")
    if decoder["maximumAggregateDecodedBytes"] > deployment["maximumTaskResidentBytes"]:
        raise AssertionError("aggregate decoded bytes exceed the resident budget")
    if decoder["maximumSingleDecoderScratchBytes"] > decoder["maximumAggregateDecodedBytes"]:
        raise AssertionError("single decoder scratch exceeds aggregate decoded bytes")
    if geometry["maximumDecodedGeometryBytes"] > decoder["maximumAggregateDecodedBytes"]:
        raise AssertionError("geometry bytes exceed aggregate decoded bytes")
    if texture["maximumDecodedRgbaBytesPerImage"] > decoder["maximumSingleDecoderScratchBytes"]:
        raise AssertionError("one decoded texture exceeds single decoder scratch")

    expected_pixels = texture["maximumWidthPixels"] * texture["maximumHeightPixels"]
    if texture["maximumPixelsPerImage"] != expected_pixels:
        raise AssertionError("texture dimensions and pixel count disagree")
    if texture["maximumDecodedRgbaBytesPerImage"] != (
        expected_pixels * RGBA_CHANNEL_COUNT
    ):
        raise AssertionError("texture RGBA bytes disagree with dimensions")
    if texture["maximumAggregateTexturePixels"] < texture["maximumPixelsPerImage"]:
        raise AssertionError("aggregate texture pixels are smaller than one image")
    if closure["maximumOutputBytes"] > deployment["maximumTaskResidentBytes"]:
        raise AssertionError("maximum output cannot fit the resident budget")
    if gltf["maximumSparseAccessorElements"] > geometry["maximumVerticesPerContent"]:
        raise AssertionError("sparse accessor elements exceed the vertex budget")
    if metadata["maximumBinaryBytes"] > decoder["maximumSingleDecoderScratchBytes"]:
        raise AssertionError("metadata binary bytes exceed single decoder scratch")
    if authorization["maximumRings"] < authorization["maximumPolygons"]:
        raise AssertionError("authorization ring budget is smaller than polygon budget")
    if authorization["maximumTriangles"] < authorization["maximumPolygons"]:
        raise AssertionError("authorization triangle budget is smaller than polygon budget")
    if decoder["maximumDracoPoints"] > geometry["maximumVerticesPerContent"]:
        raise AssertionError("Draco points exceed the canonical vertex budget")
    if decoder["maximumDracoFaces"] > geometry["maximumTrianglesPerContent"]:
        raise AssertionError("Draco faces exceed the canonical triangle budget")

    policy = profile.get("policy")
    if not isinstance(policy, dict):
        raise AssertionError("policy must be an object")
    if policy.get("limitFailureMode") != "FAIL_CLOSED":
        raise AssertionError("mesh limit failures must remain fail closed")
    if policy.get("canonicalTextureEncoding") != "PNG":
        raise AssertionError("canonical texture encoding must remain PNG")
    if policy.get("normalizationVersion") != EXPECTED_NORMALIZATION_VERSION:
        raise AssertionError("mesh normalization identity drifted")
    if policy.get("canonicalContractVersion") != EXPECTED_CANONICAL_CONTRACT_VERSION:
        raise AssertionError("mesh canonical contract identity drifted")

    print(f"validated {EXPECTED_PROFILE_ID} ({digest})")


if __name__ == "__main__":
    main()
