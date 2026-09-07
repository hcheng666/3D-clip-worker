#!/usr/bin/env python3
"""Validate Task 6 official-corpus identity, outcomes, and materialization."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


CORPUS_ROOT = Path(__file__).resolve().parent
MANIFEST_PATH = CORPUS_ROOT / "official-corpus-manifest.json"
EXPECTATIONS_PATH = CORPUS_ROOT / "mesh-phase-corpus-expectations.json"
DOWNLOAD_ROOT = CORPUS_ROOT / "downloaded"
EXPECTED_SCHEMA_VERSION = 1
EXPECTED_NORMALIZATION_VERSION = "normalization-v2"
EXPECTED_CANONICAL_CONTRACT_VERSION = "canonical-gltf2-v1"
EXPECTED_RESOURCE_PROFILE_VERSION = "STANDARD_4CPU_8GIB_SINGLE_TASK_V2"
EXPECTED_RESOURCE_PROFILE_SHA256 = (
    "bb326727f6b29a6cdd3532d85e2043c3c0ff212056b837d3d4a2eca7df3d349c"
)
REQUIRED_TEST_SLICES = {
    "GEOMETRY",
    "TEXTURE",
    "COMPRESSION",
    "TRANSFORM",
    "METADATA",
    "EMPTY_RESULT",
    "DETERMINISM",
    "RESOURCE_LIMIT",
}
REQUIRED_ENTRY_IDS = {
    "cesium-batched",
    "cesium-gltf-content",
    "khronos-avocado",
    "khronos-meshopt-cube-test",
    "khronos-stained-glass-lamp",
    "khronos-sunglasses",
}
STABLE_UNSUPPORTED_REASONS = {
    "CONTENT_VERSION_UNSUPPORTED",
    "CONTENT_PRIMITIVE_MODE_UNSUPPORTED",
    "CONTENT_GLTF_FEATURE_UNSUPPORTED",
    "COMPRESSION_DRACO_UNSUPPORTED",
    "COMPRESSION_DRACO_LIMIT_EXCEEDED",
    "COMPRESSION_MESHOPT_UNSUPPORTED",
    "COMPRESSION_MESHOPT_LIMIT_EXCEEDED",
    "TEXTURE_FORMAT_UNSUPPORTED",
    "TEXTURE_KTX2_UNSUPPORTED",
    "TEXTURE_DIMENSION_LIMIT_EXCEEDED",
    "TEXTURE_DECODED_BYTES_LIMIT_EXCEEDED",
    "METADATA_BATCH_TABLE_UNSUPPORTED",
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
    payload_files = [path for path in payload_root.rglob("*") if path.is_file()]
    if not payload_files:
        raise AssertionError(f"materialized payload is empty: {entry_id}")
    for variant in variants:
        selectors = variant.get("selectors")
        if not isinstance(selectors, list) or not selectors:
            raise AssertionError(f"variant selectors are missing: {entry_id}")
        if not any(any(payload_root.glob(selector)) for selector in selectors):
            raise AssertionError(
                f"materialized entry lacks selector evidence: "
                f"{entry_id}/{variant.get('id')}"
            )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--require-materialized",
        action="store_true",
        help="require every pinned payload and SOURCE.json to be present",
    )
    args = parser.parse_args()

    manifest = load_json(MANIFEST_PATH)
    expectations = load_json(EXPECTATIONS_PATH)
    if expectations.get("schemaVersion") != EXPECTED_SCHEMA_VERSION:
        raise AssertionError("unexpected mesh corpus expectation schema version")
    if expectations.get("normalizationVersion") != EXPECTED_NORMALIZATION_VERSION:
        raise AssertionError("mesh corpus normalization identity drifted")
    if expectations.get("canonicalContractVersion") != EXPECTED_CANONICAL_CONTRACT_VERSION:
        raise AssertionError("mesh corpus canonical contract identity drifted")
    if expectations.get("resourceProfileVersion") != EXPECTED_RESOURCE_PROFILE_VERSION:
        raise AssertionError("mesh corpus profile identity drifted")
    if expectations.get("resourceProfileSha256") != EXPECTED_RESOURCE_PROFILE_SHA256:
        raise AssertionError("mesh corpus profile hash drifted")
    if set(expectations.get("requiredTestSlices", [])) != REQUIRED_TEST_SLICES:
        raise AssertionError("mesh corpus required test slices drifted")

    known_entries = manifest_entries(manifest)
    expectation_entries = expectations.get("entries")
    if not isinstance(expectation_entries, list):
        raise AssertionError("mesh corpus entries must be an array")
    expected_ids = {
        entry.get("entryId")
        for entry in expectation_entries
        if isinstance(entry, dict)
    }
    if expected_ids != REQUIRED_ENTRY_IDS:
        raise AssertionError("mesh corpus required entry set drifted")
    if not REQUIRED_ENTRY_IDS.issubset(known_entries):
        raise AssertionError("official corpus manifest is missing a Task 6 entry")

    variant_ids: set[str] = set()
    variant_count = 0
    for entry in expectation_entries:
        if not isinstance(entry, dict):
            raise AssertionError("mesh corpus expectation entry must be an object")
        entry_id = entry["entryId"]
        variants = entry.get("variants")
        if not isinstance(variants, list) or not variants:
            raise AssertionError(f"mesh corpus variants are missing: {entry_id}")
        for variant in variants:
            if not isinstance(variant, dict):
                raise AssertionError(f"mesh corpus variant must be an object: {entry_id}")
            variant_id = variant.get("id")
            identity = f"{entry_id}:{variant_id}"
            if not isinstance(variant_id, str) or not variant_id:
                raise AssertionError(f"mesh corpus variant id is missing: {entry_id}")
            if identity in variant_ids:
                raise AssertionError(f"duplicate mesh corpus variant: {identity}")
            variant_ids.add(identity)
            outcome = variant.get("outcome")
            reason = variant.get("reason")
            if outcome == "SUPPORTED" and reason is not None:
                raise AssertionError(f"supported variant has a reason: {identity}")
            if outcome == "UNSUPPORTED" and reason not in STABLE_UNSUPPORTED_REASONS:
                raise AssertionError(f"unsupported variant lacks a stable reason: {identity}")
            if outcome not in {"SUPPORTED", "UNSUPPORTED"}:
                raise AssertionError(f"invalid mesh corpus outcome: {identity}")
            selectors = variant.get("selectors")
            if not isinstance(selectors, list) or not selectors or any(
                not isinstance(value, str) or not value for value in selectors
            ):
                raise AssertionError(f"mesh corpus selectors are invalid: {identity}")
            variant_count += 1
        if args.require_materialized:
            validate_materialized_entry(entry_id, known_entries[entry_id], variants)

    print(
        f"validated {len(REQUIRED_ENTRY_IDS)} Task 6 entries and "
        f"{variant_count} outcomes; "
        f"materialized={str(args.require_materialized).lower()}"
    )


if __name__ == "__main__":
    main()
