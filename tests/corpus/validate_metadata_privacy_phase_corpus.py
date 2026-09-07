#!/usr/bin/env python3
"""Validate Task 8 metadata corpus identities and executed result evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


CORPUS_ROOT = Path(__file__).resolve().parent
PROJECT_ROOT = CORPUS_ROOT.parents[1]
MANIFEST_PATH = CORPUS_ROOT / "official-corpus-manifest.json"
EXPECTATIONS_PATH = CORPUS_ROOT / "metadata-privacy-phase-corpus-expectations.json"
PROFILE_PATH = PROJECT_ROOT / "config" / "resource-limit-profiles-v4.json"
PROTOCOL_PATH = PROJECT_ROOT / "config" / "normalizer-protocol-v3.schema.json"
MATRIX_PATH = PROJECT_ROOT / "config" / "metadata-capability-matrix-v1.json"
DOWNLOAD_ROOT = CORPUS_ROOT / "downloaded"
RESULT_NAME = "RESULTS.metadata-v1.json"
EXPECTED_SCHEMA_VERSION = 1
EXPECTED_PROTOCOL_VERSION = "THREE_D_TILES_NORMALIZER_V3"
EXPECTED_PROTOCOL_SHA256 = (
    "faf33e1c023ef5cdbd3cc41285dec3e7d8065ce59ffd9cf623ca72f3e9f3acb6"
)
EXPECTED_PROFILE_VERSION = "STANDARD_4CPU_8GIB_SINGLE_TASK_V4"
EXPECTED_PROFILE_SHA256 = (
    "b1a631c3f776210362ff795737603af88578a10634a04713dc04adff69880999"
)
EXPECTED_MATRIX_VERSION = "THREE_D_TILES_METADATA_CAPABILITY_V1"
EXPECTED_MATRIX_SHA256 = (
    "3be3a6b7431fd1345ce09b27bacefc4c9095fb307a6eb5fa79496cc450d65090"
)
EXPECTED_FAMILY_VERSIONS = {
    "MESH_GLTF2": {
        "normalizationVersion": "normalization-mesh-metadata-v1",
        "canonicalContractVersion": "canonical-mesh-metadata-gltf2-v1",
    },
    "POINT_GLTF2": {
        "normalizationVersion": "normalization-point-metadata-v1",
        "canonicalContractVersion": "canonical-point-metadata-gltf2-v1",
    },
    "INSTANCE_GLTF2": {
        "normalizationVersion": "normalization-instance-metadata-v1",
        "canonicalContractVersion": "canonical-instance-metadata-gltf2-v1",
    },
    "COMPOSITE_CHILDREN": {
        "normalizationVersion": "normalization-composite-metadata-v1",
        "canonicalContractVersion": "canonical-composite-children-metadata-v1",
    },
}
REQUIRED_ENTRY_IDS = {
    "cesium-metadata",
    "validator-full-metadata",
    "validator-invalid-feature-id",
}
REQUIRED_ASSERTIONS = {
    "PARSER",
    "NORMALIZER",
    "PROPERTY_LOOKUP",
    "REMOVED_ID_REJECTION",
    "LEAKAGE_SCAN",
    "DETERMINISM",
}
STABLE_REASONS = {
    "METADATA_FEATURE_ID_INVALID",
    "METADATA_PROPERTY_ATTRIBUTE_UNSUPPORTED",
    "METADATA_RELATIONSHIP_UNSUPPORTED",
    "METADATA_STATISTICS_UNSUPPORTED",
}


def load_json(path: Path) -> dict[str, object]:
    with path.open("r", encoding="utf-8-sig") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise AssertionError(f"{path} must contain a JSON object")
    return value


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def manifest_entries(manifest: dict[str, object]) -> dict[str, dict[str, str]]:
    result: dict[str, dict[str, str]] = {}
    for source in manifest.get("sources", []):
        if not isinstance(source, dict):
            raise AssertionError("manifest source must be an object")
        for entry in source.get("entries", []):
            if not isinstance(entry, dict) or not isinstance(entry.get("id"), str):
                raise AssertionError("manifest entry identity is incomplete")
            entry_id = entry["id"]
            if entry_id in result:
                raise AssertionError(f"duplicate manifest entry: {entry_id}")
            result[entry_id] = {
                "sourceId": source["id"],
                "repository": source["repository"],
                "revision": source["revision"],
                "sourcePath": entry["path"],
            }
    return result


def validate_executed_results(
    entry_id: str,
    expected_source: dict[str, str],
    variants: list[dict[str, object]],
) -> None:
    entry_root = DOWNLOAD_ROOT / entry_id
    source_path = entry_root / "SOURCE.json"
    payload_root = entry_root / "payload"
    result_path = entry_root / RESULT_NAME
    if not source_path.is_file() or not payload_root.exists():
        raise AssertionError(f"official corpus entry is not materialized: {entry_id}")
    if not result_path.is_file():
        raise AssertionError(
            f"executed metadata result evidence is missing: {entry_id}/{RESULT_NAME}"
        )
    source = load_json(source_path)
    for field in ("sourceId", "repository", "revision", "sourcePath"):
        if source.get(field) != expected_source[field]:
            raise AssertionError(f"materialized {entry_id} {field} drifted")
    results = load_json(result_path)
    if results.get("entryId") != entry_id:
        raise AssertionError(f"metadata results entry identity drifted: {entry_id}")
    if results.get("revision") != expected_source["revision"]:
        raise AssertionError(f"metadata results revision drifted: {entry_id}")
    actual_variants = results.get("variants")
    if not isinstance(actual_variants, list):
        raise AssertionError(f"metadata results variants are absent: {entry_id}")
    by_id = {
        value.get("id"): value
        for value in actual_variants
        if isinstance(value, dict) and isinstance(value.get("id"), str)
    }
    for expected in variants:
        variant_id = expected["id"]
        actual = by_id.get(variant_id)
        if actual is None:
            raise AssertionError(f"metadata variant was not executed: {entry_id}/{variant_id}")
        if actual.get("outcome") != expected["outcome"]:
            raise AssertionError(f"metadata outcome drifted: {entry_id}/{variant_id}")
        if actual.get("reason") != expected.get("reason"):
            raise AssertionError(f"metadata reason drifted: {entry_id}/{variant_id}")
        assertions = actual.get("assertions")
        if set(assertions) != REQUIRED_ASSERTIONS:
            raise AssertionError(
                f"metadata execution assertions are incomplete: {entry_id}/{variant_id}"
            )
        if actual.get("passed") is not True:
            raise AssertionError(f"metadata execution failed: {entry_id}/{variant_id}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--require-materialized",
        action="store_true",
        help="require pinned payloads plus parser/normalizer/lookup/leakage results",
    )
    args = parser.parse_args()

    expectations = load_json(EXPECTATIONS_PATH)
    if expectations.get("schemaVersion") != EXPECTED_SCHEMA_VERSION:
        raise AssertionError("unexpected Task 8 corpus expectation schema version")
    if expectations.get("protocolVersion") != EXPECTED_PROTOCOL_VERSION:
        raise AssertionError("Task 8 protocol identity drifted")
    if expectations.get("protocolSchemaSha256") != EXPECTED_PROTOCOL_SHA256:
        raise AssertionError("Task 8 protocol hash declaration drifted")
    if sha256(PROTOCOL_PATH) != EXPECTED_PROTOCOL_SHA256:
        raise AssertionError("Task 8 protocol schema bytes drifted")
    if expectations.get("resourceProfileVersion") != EXPECTED_PROFILE_VERSION:
        raise AssertionError("Task 8 resource profile identity drifted")
    if expectations.get("resourceProfileSha256") != EXPECTED_PROFILE_SHA256:
        raise AssertionError("Task 8 resource profile hash declaration drifted")
    if sha256(PROFILE_PATH) != EXPECTED_PROFILE_SHA256:
        raise AssertionError("Task 8 resource profile bytes drifted")
    if expectations.get("metadataMatrixVersion") != EXPECTED_MATRIX_VERSION:
        raise AssertionError("Task 8 metadata matrix identity drifted")
    if expectations.get("metadataMatrixSha256") != EXPECTED_MATRIX_SHA256:
        raise AssertionError("Task 8 metadata matrix hash declaration drifted")
    if sha256(MATRIX_PATH) != EXPECTED_MATRIX_SHA256:
        raise AssertionError("Task 8 metadata matrix bytes drifted")
    if expectations.get("familyVersions") != EXPECTED_FAMILY_VERSIONS:
        raise AssertionError("Task 8 family version identities drifted")
    if set(expectations.get("requiredAssertions", [])) != REQUIRED_ASSERTIONS:
        raise AssertionError("Task 8 required assertions drifted")

    known_entries = manifest_entries(load_json(MANIFEST_PATH))
    entries = expectations.get("entries")
    if not isinstance(entries, list):
        raise AssertionError("Task 8 corpus entries must be an array")
    entry_ids = {entry.get("entryId") for entry in entries if isinstance(entry, dict)}
    if entry_ids != REQUIRED_ENTRY_IDS or not REQUIRED_ENTRY_IDS.issubset(known_entries):
        raise AssertionError("Task 8 official corpus entry set drifted")

    covered_assertions: set[str] = set()
    variant_count = 0
    identities: set[str] = set()
    for entry in entries:
        if not isinstance(entry, dict):
            raise AssertionError("Task 8 corpus entry must be an object")
        entry_id = entry["entryId"]
        variants = entry.get("variants")
        if not isinstance(variants, list) or not variants:
            raise AssertionError(f"Task 8 variants are missing: {entry_id}")
        for variant in variants:
            if not isinstance(variant, dict):
                raise AssertionError(f"Task 8 variant must be an object: {entry_id}")
            variant_id = variant.get("id")
            identity = f"{entry_id}:{variant_id}"
            if not isinstance(variant_id, str) or not variant_id or identity in identities:
                raise AssertionError(f"Task 8 variant identity is invalid: {identity}")
            identities.add(identity)
            selectors = variant.get("selectors")
            if not isinstance(selectors, list) or not selectors or any(
                not isinstance(value, str) or not value for value in selectors
            ):
                raise AssertionError(f"Task 8 selectors are invalid: {identity}")
            outcome = variant.get("outcome")
            reason = variant.get("reason")
            if outcome == "SUPPORTED" and reason is not None:
                raise AssertionError(f"supported Task 8 variant has a reason: {identity}")
            if outcome == "UNSUPPORTED" and reason not in STABLE_REASONS:
                raise AssertionError(f"unsupported Task 8 variant reason is invalid: {identity}")
            if outcome not in {"SUPPORTED", "UNSUPPORTED"}:
                raise AssertionError(f"Task 8 variant outcome is invalid: {identity}")
            variant_count += 1
        if args.require_materialized:
            validate_executed_results(entry_id, known_entries[entry_id], variants)

    evidence = expectations.get("nativeEvidence")
    if not isinstance(evidence, list) or not evidence:
        raise AssertionError("Task 8 native evidence is missing")
    for item in evidence:
        if not isinstance(item, dict) or not isinstance(item.get("test"), str):
            raise AssertionError("Task 8 native evidence identity is invalid")
        assertions = item.get("assertions")
        if not isinstance(assertions, list) or not assertions:
            raise AssertionError("Task 8 native evidence assertions are missing")
        covered_assertions.update(assertions)
    if covered_assertions != REQUIRED_ASSERTIONS:
        raise AssertionError("Task 8 native evidence coverage drifted")

    print(
        f"validated {len(REQUIRED_ENTRY_IDS)} Task 8 entries and "
        f"{variant_count} outcomes; "
        f"materialized={str(args.require_materialized).lower()}"
    )


if __name__ == "__main__":
    main()
