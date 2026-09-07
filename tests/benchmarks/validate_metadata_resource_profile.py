#!/usr/bin/env python3
"""Validate the hash-closed Task 8 metadata-safe resource profile."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
PROFILE_PATH = PROJECT_ROOT / "config" / "resource-limit-profiles-v4.json"
EXPECTED_SCHEMA_VERSION = 4
EXPECTED_PROFILE_ID = "STANDARD_4CPU_8GIB_SINGLE_TASK_V4"
EXPECTED_PROFILE_SHA256 = (
    "b1a631c3f776210362ff795737603af88578a10634a04713dc04adff69880999"
)
EXPECTED_POLICY = {
    "limitFailureMode": "FAIL_CLOSED",
    "meshNormalizationVersion": "normalization-mesh-metadata-v1",
    "meshCanonicalContractVersion": "canonical-mesh-metadata-gltf2-v1",
    "pointNormalizationVersion": "normalization-point-metadata-v1",
    "pointCanonicalContractVersion": "canonical-point-metadata-gltf2-v1",
    "instanceNormalizationVersion": "normalization-instance-metadata-v1",
    "instanceCanonicalContractVersion": "canonical-instance-metadata-gltf2-v1",
    "compositeNormalizationVersion": "normalization-composite-metadata-v1",
    "compositeCanonicalContractVersion": (
        "canonical-composite-children-metadata-v1"
    ),
}
MAXIMUM_COMPOSITE_DEPTH = 64


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
        raise AssertionError(f"Task 8 resource profile hash drifted: {digest}")

    registry = load_json(PROFILE_PATH)
    if registry.get("schemaVersion") != EXPECTED_SCHEMA_VERSION:
        raise AssertionError("unexpected Task 8 resource profile schema version")
    if registry.get("defaultProfile") != EXPECTED_PROFILE_ID:
        raise AssertionError("the approved Task 8 profile must remain the default")

    profiles = registry.get("profiles")
    if not isinstance(profiles, list) or len(profiles) != 1:
        raise AssertionError("Task 8 profile registry must contain one pinned profile")
    profile = profiles[0]
    if not isinstance(profile, dict) or profile.get("id") != EXPECTED_PROFILE_ID:
        raise AssertionError("approved Task 8 resource profile is missing")

    for section_name in ("point", "instance", "composite", "metadata"):
        section = profile.get(section_name)
        if not isinstance(section, dict):
            raise AssertionError(f"{section_name} must be an object")
        require_positive_integers(section, section_name)

    point = profile["point"]
    instance = profile["instance"]
    composite = profile["composite"]
    metadata = profile["metadata"]
    if point["maximumDecodedBytes"] < metadata["maximumBinaryBytes"]:
        raise AssertionError("point decoded budget cannot contain metadata binary")
    if instance["maximumDecodedBytes"] < metadata["maximumBinaryBytes"]:
        raise AssertionError("instance decoded budget cannot contain metadata binary")
    if instance["maximumBoundaryInstances"] > instance["maximumInstances"]:
        raise AssertionError("boundary instance count exceeds total instances")
    if composite["maximumOutputs"] != composite["maximumChildren"] + 1:
        raise AssertionError("composite output budget must include one parent manifest")
    if composite["maximumDepth"] > MAXIMUM_COMPOSITE_DEPTH:
        raise AssertionError("composite recursion depth exceeds the safety ceiling")
    if metadata["maximumFeatureRows"] < max(
        point["maximumPoints"], instance["maximumInstances"]
    ):
        raise AssertionError("metadata rows cannot cover the geometry limits")
    if metadata["maximumHierarchyInstances"] > metadata["maximumFeatureRows"]:
        raise AssertionError("hierarchy instances exceed the feature-row budget")
    if metadata["maximumHierarchyEdges"] < metadata["maximumHierarchyInstances"]:
        raise AssertionError("hierarchy edge budget cannot cover one parent per row")
    if metadata["maximumFeatureMappingEntries"] < metadata["maximumFeatureRows"]:
        raise AssertionError("feature mapping budget cannot cover retained rows")
    if metadata["maximumAncestorClosureEntries"] < metadata["maximumHierarchyEdges"]:
        raise AssertionError("ancestor closure budget cannot cover hierarchy edges")
    if metadata["maximumValuesBytes"] > metadata["maximumBinaryBytes"]:
        raise AssertionError("typed values exceed the aggregate metadata binary budget")
    if metadata["maximumDecodedStringBytes"] > metadata["maximumStringBytes"]:
        raise AssertionError("decoded strings exceed the string byte budget")
    if metadata["maximumValidationOperations"] < max(
        metadata["maximumFeatureMappingEntries"],
        metadata["maximumAncestorClosureEntries"],
    ):
        raise AssertionError("validation operations cannot cover one full metadata pass")
    if profile.get("policy") != EXPECTED_POLICY:
        raise AssertionError("Task 8 version or fail-closed policy drifted")

    print(f"validated {EXPECTED_PROFILE_ID} at {digest}")


if __name__ == "__main__":
    main()
