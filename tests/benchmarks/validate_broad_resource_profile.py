#!/usr/bin/env python3
"""Validate the pinned Task 7 point/instance/composite resource profile."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
PROFILE_PATH = PROJECT_ROOT / "config" / "resource-limit-profiles-v3.json"
EXPECTED_SCHEMA_VERSION = 3
EXPECTED_PROFILE_ID = "STANDARD_4CPU_8GIB_SINGLE_TASK_V3"
EXPECTED_PROFILE_SHA256 = (
    "d6097a29380375180e2cf9374eae354a9672e0589f3fb04b2f85930f16d5af78"
)
EXPECTED_POLICY = {
    "limitFailureMode": "FAIL_CLOSED",
    "pointNormalizationVersion": "normalization-point-v1",
    "pointCanonicalContractVersion": "canonical-point-gltf2-v1",
    "instanceNormalizationVersion": "normalization-instance-v1",
    "instanceCanonicalContractVersion": "canonical-instance-gltf2-v1",
    "compositeNormalizationVersion": "normalization-composite-v1",
    "compositeCanonicalContractVersion": "canonical-composite-children-v1",
}


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
    raw = PROFILE_PATH.read_bytes()
    if hashlib.sha256(raw).hexdigest() != EXPECTED_PROFILE_SHA256:
        raise AssertionError("Task 7 resource profile hash drifted")

    registry = load_json(PROFILE_PATH)
    if registry.get("schemaVersion") != EXPECTED_SCHEMA_VERSION:
        raise AssertionError("unexpected Task 7 resource profile schema version")
    if registry.get("defaultProfile") != EXPECTED_PROFILE_ID:
        raise AssertionError("the approved Task 7 profile must remain the default")

    profiles = registry.get("profiles")
    if not isinstance(profiles, list) or len(profiles) != 1:
        raise AssertionError("Task 7 profile registry must contain one pinned profile")
    profile = profiles[0]
    if not isinstance(profile, dict) or profile.get("id") != EXPECTED_PROFILE_ID:
        raise AssertionError("approved Task 7 resource profile is missing")

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
        raise AssertionError("point decoded budget cannot contain metadata binary budget")
    if instance["maximumDecodedBytes"] < metadata["maximumBinaryBytes"]:
        raise AssertionError("instance decoded budget cannot contain metadata binary budget")
    if instance["maximumBoundaryInstances"] > instance["maximumInstances"]:
        raise AssertionError("boundary instance count exceeds total instance count")
    if composite["maximumOutputs"] != composite["maximumChildren"] + 1:
        raise AssertionError("composite output budget must include one parent manifest")
    if composite["maximumDepth"] > 64:
        raise AssertionError("composite recursion depth exceeds the hard safety ceiling")
    if metadata["maximumFeatureRows"] < max(
        point["maximumPoints"], instance["maximumInstances"]
    ):
        raise AssertionError("metadata feature rows cannot cover the geometry limits")
    if profile.get("policy") != EXPECTED_POLICY:
        raise AssertionError("Task 7 version or fail-closed policy drifted")

    print(f"validated {EXPECTED_PROFILE_ID} at {EXPECTED_PROFILE_SHA256}")


if __name__ == "__main__":
    main()
