#!/usr/bin/env python3
"""Validate the named Worker resource profile and its benchmark evidence."""

from __future__ import annotations

import json
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
PROFILE_PATH = PROJECT_ROOT / "config" / "resource-limit-profiles-v1.json"
BENCHMARK_PATH = PROJECT_ROOT / "tests" / "benchmarks" / "results" / "resource-limit-baseline.json"
EXPECTED_SCHEMA_VERSION = 1
EXPECTED_PROFILE_ID = "STANDARD_4CPU_8GIB_SINGLE_TASK_V1"
RGBA_CHANNEL_COUNT = 4


def require_positive_integers(values: dict[str, object], section: str) -> None:
    for name, value in values.items():
        if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
            raise AssertionError(f"{section}.{name} must be a positive integer")


def load_json(path: Path) -> dict[str, object]:
    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise AssertionError(f"{path} must contain a JSON object")
    return value


def main() -> None:
    registry = load_json(PROFILE_PATH)
    if registry.get("schemaVersion") != EXPECTED_SCHEMA_VERSION:
        raise AssertionError("unexpected resource profile schema version")
    if registry.get("defaultProfile") != EXPECTED_PROFILE_ID:
        raise AssertionError("the approved standard profile must remain the default")

    profiles = registry.get("profiles")
    if not isinstance(profiles, list) or not profiles:
        raise AssertionError("profiles must be a non-empty array")
    profile_ids = [profile.get("id") for profile in profiles if isinstance(profile, dict)]
    if len(profile_ids) != len(set(profile_ids)):
        raise AssertionError("resource profile ids must be unique")
    profile = next(
        (item for item in profiles if isinstance(item, dict) and item.get("id") == EXPECTED_PROFILE_ID),
        None,
    )
    if profile is None:
        raise AssertionError("approved standard resource profile is missing")

    numeric_sections = (
        "deployment",
        "archive",
        "resourceClosure",
        "inspectorProtocol",
        "geometry",
        "texture",
        "decoder",
        "deadlineSeconds",
    )
    for section_name in numeric_sections:
        section = profile.get(section_name)
        if not isinstance(section, dict):
            raise AssertionError(f"{section_name} must be an object")
        require_positive_integers(section, section_name)

    deployment = profile["deployment"]
    decoder = profile["decoder"]
    texture = profile["texture"]
    archive = profile["archive"]
    protocol = profile["inspectorProtocol"]
    if deployment["maximumConcurrentTasks"] != 1:
        raise AssertionError("the standard profile must remain single-task")
    if deployment["maximumTaskResidentBytes"] >= deployment["memoryBytes"]:
        raise AssertionError("the task budget must leave container runtime headroom")
    if decoder["maximumAggregateDecodedBytes"] > deployment["maximumTaskResidentBytes"]:
        raise AssertionError("aggregate decoder bytes exceed the task resident budget")
    if decoder["maximumSingleDecoderScratchBytes"] > decoder["maximumAggregateDecodedBytes"]:
        raise AssertionError("single decoder scratch exceeds the aggregate decoder budget")
    expected_rgba_bytes = (
        texture["maximumWidthPixels"]
        * texture["maximumHeightPixels"]
        * RGBA_CHANNEL_COUNT
    )
    if texture["maximumPixelsPerImage"] != (
        texture["maximumWidthPixels"] * texture["maximumHeightPixels"]
    ):
        raise AssertionError("texture dimensions and pixel count disagree")
    if texture["maximumDecodedRgbaBytesPerImage"] != expected_rgba_bytes:
        raise AssertionError("texture RGBA byte limit disagrees with dimensions")
    if archive["maximumEntryBytes"] > archive["maximumExpandedBytes"]:
        raise AssertionError("single archive entry exceeds total expanded byte limit")
    if protocol["maximumManifestPageRecords"] > archive["maximumEntryCount"]:
        raise AssertionError("manifest page records exceed the package entry limit")
    if protocol["maximumResultPageRecords"] > profile["resourceClosure"]["maximumResourceCount"]:
        raise AssertionError("result page records exceed the resource closure limit")
    if protocol["maximumPageJsonBytes"] >= deployment["maximumTaskResidentBytes"]:
        raise AssertionError("protocol page bytes do not leave task memory headroom")
    if protocol["maximumDiagnosticMessageUtf8Bytes"] > protocol["maximumDiagnosticJsonBytes"]:
        raise AssertionError("one diagnostic message exceeds the total diagnostic budget")
    if protocol["maximumDependenciesPerResource"] > profile["resourceClosure"]["maximumResourceCount"]:
        raise AssertionError("per-resource dependencies exceed the closure resource limit")
    if protocol["minimumGrantLifetimeSeconds"] >= protocol["maximumGrantLifetimeSeconds"]:
        raise AssertionError("Inspector grant lifetime bounds are inconsistent")
    if protocol["maximumGrantLifetimeSeconds"] < profile["deadlineSeconds"]["inspection"]:
        raise AssertionError("Inspector grant lifetime cannot cover the inspection deadline")

    evidence = profile.get("evidence")
    benchmark = load_json(BENCHMARK_PATH)
    if not isinstance(evidence, dict):
        raise AssertionError("profile benchmark evidence must be an object")
    if evidence.get("reportPath") != BENCHMARK_PATH.relative_to(PROJECT_ROOT).as_posix():
        raise AssertionError("profile benchmark path does not identify the committed report")
    if evidence.get("benchmarkVersion") != benchmark.get("benchmarkVersion"):
        raise AssertionError("profile and benchmark versions disagree")
    if benchmark.get("candidateUriDepth") != profile["resourceClosure"]["maximumUriDepth"]:
        raise AssertionError("measured URI-depth candidate and profile limit disagree")
    if max(item["scale"] for item in benchmark["inspection"]) > archive["maximumEntryCount"]:
        raise AssertionError("benchmark entry scale exceeds the configured entry limit")
    if max(item["scale"] for item in benchmark["normalization"]) > profile["resourceClosure"]["maximumResourceCount"]:
        raise AssertionError("benchmark resource scale exceeds the configured resource limit")

    print(f"validated {EXPECTED_PROFILE_ID} against {benchmark['benchmarkVersion']}")


if __name__ == "__main__":
    main()
