#!/usr/bin/env python3
"""Validate Task 7 corpus identity, coverage, and optional materialization."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


CORPUS_ROOT = Path(__file__).resolve().parent
PROJECT_ROOT = CORPUS_ROOT.parents[1]
MANIFEST_PATH = CORPUS_ROOT / "official-corpus-manifest.json"
EXPECTATIONS_PATH = (
    CORPUS_ROOT / "point-instance-composite-phase-corpus-expectations.json"
)
PROFILE_PATH = PROJECT_ROOT / "config" / "resource-limit-profiles-v3.json"
DOWNLOAD_ROOT = CORPUS_ROOT / "downloaded"
EXPECTED_SCHEMA_VERSION = 1
EXPECTED_PROTOCOL_VERSION = "THREE_D_TILES_NORMALIZER_V2"
EXPECTED_PROFILE_VERSION = "STANDARD_4CPU_8GIB_SINGLE_TASK_V3"
EXPECTED_PROFILE_SHA256 = (
    "d6097a29380375180e2cf9374eae354a9672e0589f3fb04b2f85930f16d5af78"
)
EXPECTED_FAMILY_VERSIONS = {
    "POINT_GLTF2": {
        "normalizationVersion": "normalization-point-v1",
        "canonicalContractVersion": "canonical-point-gltf2-v1",
    },
    "INSTANCE_GLTF2": {
        "normalizationVersion": "normalization-instance-v1",
        "canonicalContractVersion": "canonical-instance-gltf2-v1",
    },
    "COMPOSITE_CHILDREN": {
        "normalizationVersion": "normalization-composite-v1",
        "canonicalContractVersion": "canonical-composite-children-v1",
    },
}
REQUIRED_ENTRY_IDS = {
    "cesium-point-cloud",
    "cesium-instanced",
    "cesium-composite",
}
REQUIRED_TEST_SLICES = {
    "POSITION_QUANTIZATION",
    "COLOR",
    "NORMAL",
    "RTC_CENTER",
    "ORIENTATION",
    "SCALE",
    "EXTERNAL_MODEL",
    "METADATA_COMPACTION",
    "POINT_EXACT_CLIP",
    "INSTANCE_CONSERVATIVE_CLIP",
    "BOUNDARY_EXPANSION",
    "NESTED_COMPOSITE",
    "MIXED_UNSUPPORTED_CHILD",
    "EMPTY_RESULT",
    "DETERMINISM",
    "RESOURCE_LIMIT",
}
STABLE_UNSUPPORTED_REASONS = {
    "PNTS_SEMANTIC_UNSUPPORTED",
    "I3DM_SEMANTIC_UNSUPPORTED",
    "I3DM_ENU_UNSUPPORTED",
    "CMPT_CHILD_UNSUPPORTED",
}


def load_json(path: Path) -> dict[str, object]:
    with path.open("r", encoding="utf-8-sig") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise AssertionError(f"{path} must contain a JSON object")
    return value


def manifest_entries(manifest: dict[str, object]) -> dict[str, dict[str, str]]:
    result: dict[str, dict[str, str]] = {}
    for source in manifest.get("sources", []):
        if not isinstance(source, dict):
            raise AssertionError("manifest source must be an object")
        source_id = source.get("id")
        repository = source.get("repository")
        revision = source.get("revision")
        if not all(isinstance(value, str) and value for value in (
            source_id, repository, revision
        )):
            raise AssertionError("manifest source identity is incomplete")
        for entry in source.get("entries", []):
            if not isinstance(entry, dict) or not isinstance(entry.get("id"), str):
                raise AssertionError("manifest entry identity is incomplete")
            entry_id = entry["id"]
            if entry_id in result:
                raise AssertionError(f"duplicate manifest entry: {entry_id}")
            result[entry_id] = {
                "sourceId": source_id,
                "repository": repository,
                "revision": revision,
                "sourcePath": entry.get("path"),
            }
    return result


def validate_materialized_entry(
    entry_id: str,
    expected_source: dict[str, str],
    variants: list[dict[str, object]],
) -> None:
    entry_root = DOWNLOAD_ROOT / entry_id
    source_path = entry_root / "SOURCE.json"
    payload_root = entry_root / "payload"
    if not source_path.is_file() or not payload_root.exists():
        raise AssertionError(f"official corpus entry is not materialized: {entry_id}")
    source = load_json(source_path)
    for field in ("entryId", "sourceId", "repository", "revision", "sourcePath"):
        expected = entry_id if field == "entryId" else expected_source[field]
        if source.get(field) != expected:
            raise AssertionError(f"materialized {entry_id} {field} drifted")
    for variant in variants:
        selectors = variant["selectors"]
        if not any(any(payload_root.glob(selector)) for selector in selectors):
            raise AssertionError(
                f"materialized entry lacks selector evidence: "
                f"{entry_id}/{variant['id']}"
            )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--require-materialized",
        action="store_true",
        help="require all three pinned Cesium payloads and SOURCE.json files",
    )
    args = parser.parse_args()

    expectations = load_json(EXPECTATIONS_PATH)
    if expectations.get("schemaVersion") != EXPECTED_SCHEMA_VERSION:
        raise AssertionError("unexpected Task 7 corpus expectation schema version")
    if expectations.get("protocolVersion") != EXPECTED_PROTOCOL_VERSION:
        raise AssertionError("Task 7 normalizer protocol identity drifted")
    if expectations.get("resourceProfileVersion") != EXPECTED_PROFILE_VERSION:
        raise AssertionError("Task 7 resource profile identity drifted")
    if expectations.get("resourceProfileSha256") != EXPECTED_PROFILE_SHA256:
        raise AssertionError("Task 7 resource profile hash declaration drifted")
    if hashlib.sha256(PROFILE_PATH.read_bytes()).hexdigest() != EXPECTED_PROFILE_SHA256:
        raise AssertionError("Task 7 resource profile bytes drifted")
    if expectations.get("familyVersions") != EXPECTED_FAMILY_VERSIONS:
        raise AssertionError("Task 7 per-family version identities drifted")
    if set(expectations.get("requiredTestSlices", [])) != REQUIRED_TEST_SLICES:
        raise AssertionError("Task 7 required test slices drifted")

    known_entries = manifest_entries(load_json(MANIFEST_PATH))
    entries = expectations.get("entries")
    if not isinstance(entries, list):
        raise AssertionError("Task 7 corpus entries must be an array")
    entry_ids = {entry.get("entryId") for entry in entries if isinstance(entry, dict)}
    if entry_ids != REQUIRED_ENTRY_IDS:
        raise AssertionError("Task 7 official corpus entry set drifted")
    if not REQUIRED_ENTRY_IDS.issubset(known_entries):
        raise AssertionError("official corpus manifest is missing a Task 7 entry")

    covered_slices: set[str] = set()
    variant_ids: set[str] = set()
    variant_count = 0
    for entry in entries:
        if not isinstance(entry, dict):
            raise AssertionError("Task 7 corpus entry must be an object")
        entry_id = entry["entryId"]
        variants = entry.get("variants")
        if not isinstance(variants, list) or not variants:
            raise AssertionError(f"Task 7 corpus variants are missing: {entry_id}")
        for variant in variants:
            if not isinstance(variant, dict):
                raise AssertionError(f"Task 7 corpus variant must be an object: {entry_id}")
            variant_id = variant.get("id")
            if not isinstance(variant_id, str) or not variant_id:
                raise AssertionError(f"Task 7 corpus variant id is missing: {entry_id}")
            identity = f"{entry_id}:{variant_id}"
            if identity in variant_ids:
                raise AssertionError(f"duplicate Task 7 corpus variant: {identity}")
            variant_ids.add(identity)
            selectors = variant.get("selectors")
            slices = variant.get("slices")
            if not isinstance(selectors, list) or not selectors or any(
                not isinstance(value, str) or not value for value in selectors
            ):
                raise AssertionError(f"Task 7 selectors are invalid: {identity}")
            if not isinstance(slices, list) or not slices:
                raise AssertionError(f"Task 7 slices are missing: {identity}")
            outcome = variant.get("outcome")
            reason = variant.get("reason")
            if outcome == "SUPPORTED" and reason is not None:
                raise AssertionError(f"supported Task 7 variant has a reason: {identity}")
            if outcome == "UNSUPPORTED" and reason not in STABLE_UNSUPPORTED_REASONS:
                raise AssertionError(f"unsupported Task 7 variant lacks a stable reason: {identity}")
            if outcome not in {"SUPPORTED", "UNSUPPORTED"}:
                raise AssertionError(f"invalid Task 7 corpus outcome: {identity}")
            covered_slices.update(slices)
            variant_count += 1
        if args.require_materialized:
            validate_materialized_entry(entry_id, known_entries[entry_id], variants)

    native_evidence = expectations.get("nativeEvidence")
    if not isinstance(native_evidence, list) or not native_evidence:
        raise AssertionError("Task 7 native evidence is missing")
    for evidence in native_evidence:
        if not isinstance(evidence, dict):
            raise AssertionError("Task 7 native evidence must be an object")
        test_name = evidence.get("test")
        slices = evidence.get("slices")
        if not isinstance(test_name, str) or not test_name:
            raise AssertionError("Task 7 native evidence test name is missing")
        if not isinstance(slices, list) or not slices:
            raise AssertionError(f"Task 7 native evidence slices are missing: {test_name}")
        covered_slices.update(slices)
    if covered_slices != REQUIRED_TEST_SLICES:
        raise AssertionError("Task 7 corpus/native evidence coverage drifted")

    print(
        f"validated {len(REQUIRED_ENTRY_IDS)} Task 7 entries and "
        f"{variant_count} outcomes; "
        f"materialized={str(args.require_materialized).lower()}"
    )


if __name__ == "__main__":
    main()
