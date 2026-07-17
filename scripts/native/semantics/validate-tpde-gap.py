#!/usr/bin/env python3
"""Validate the W01 TPDE capability inventory against a pinned checkout."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path, PurePosixPath
from typing import Any


TPDE_COMMIT = "338d41890e424b058e2053b6a5787e1348e3dd57"
CLASSIFICATIONS = {
    "covered",
    "php_shim",
    "tpde_extension",
    "tpde_core_patch",
    "blocked",
    "not_needed",
}
OWNERS = {"php", "tpde", "shared"}
UPSTREAMABILITY = {
    "not_applicable",
    "php_local",
    "good_candidate",
    "requires_core_review",
}
TARGET_STATUSES = {"required", "analyzed_unreleased", "not_applicable"}
CRITICAL_IDS = {
    "ir-adaptor-ssa-phi",
    "single-entry-functions",
    "multipart-values",
    "calls-calling-conventions",
    "fixed-context-registers",
    "branches-switches",
    "symbols-relocations",
    "elf-object-output",
    "custom-code-allocation",
    "custom-mapper",
    "persistable-relocations",
    "process-local-symbol-resolution",
    "wx-mapping",
    "aarch64-icache",
    "aarch64-branch-veneers",
    "unwind-eh-frame",
    "statepoint-code-offset",
    "force-materialize",
    "value-location-snapshot",
    "frame-stackmap-metadata",
    "resume-osr",
    "debug-perf",
    "thread-safety",
    "reset-reuse",
    "cxx20-no-eh",
    "x86-64-target",
    "aarch64-target",
}
MANDATORY_SOURCE_FILES = {
    "README.md",
    "docs/tpde/adaptor.md",
    "docs/tpde/overview.md",
    "docs/tpde/compiler-ref.md",
    "tpde/include/tpde/IRAdaptor.hpp",
    "tpde/include/tpde/Compiler.hpp",
    "tpde/include/tpde/CompilerBase.hpp",
    "tpde/include/tpde/ValueAssignment.hpp",
    "tpde/include/tpde/AssignmentPartRef.hpp",
    "tpde/include/tpde/AssemblerElf.hpp",
    "tpde/include/tpde/ElfMapper.hpp",
    "tpde/src/ElfMapper.cpp",
    "tpde/include/tpde/FunctionWriter.hpp",
    "tpde/include/tpde/x64/CompilerX64.hpp",
    "tpde/include/tpde/arm64/CompilerA64.hpp",
    "tpde/CMakeLists.txt",
    "CMakeLists.txt",
}
TOP_LEVEL_FIELDS = {
    "format_version",
    "repository",
    "tpde_commit",
    "primary_target",
    "secondary_target",
    "classifications",
    "reviewed_source_files",
    "capabilities",
}
CAPABILITY_FIELDS = {
    "id",
    "critical",
    "php_requirement",
    "motivating_contract",
    "classification",
    "source_refs",
    "existing_behavior",
    "missing_behavior",
    "minimal_api_or_shim",
    "owner",
    "upstreamability",
    "target_impact",
    "required_tests",
    "blocker_id",
    "decision_id",
}
SOURCE_REF_FIELDS = {"path", "symbol", "start_line", "end_line", "evidence"}
TARGET_FIELDS = {"status", "impact"}


def _nonempty_string(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


def _run_git(tpde: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", "-C", str(tpde), *args],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def validate_document(data: Any, tpde: Path | None = None) -> list[str]:
    """Return deterministic validation errors for a decoded manifest."""
    errors: list[str] = []
    if not isinstance(data, dict):
        return ["document must be a JSON object"]

    missing_fields = sorted(TOP_LEVEL_FIELDS - set(data))
    extra_fields = sorted(set(data) - TOP_LEVEL_FIELDS)
    if missing_fields:
        errors.append("missing top-level fields: " + ", ".join(missing_fields))
    if extra_fields:
        errors.append("unrecognized top-level fields: " + ", ".join(extra_fields))

    if data.get("format_version") != "1.0.0":
        errors.append("format_version must be 1.0.0")
    if data.get("repository") != "tpde2/tpde":
        errors.append("repository must be tpde2/tpde")
    if data.get("tpde_commit") != TPDE_COMMIT:
        errors.append(f"tpde_commit must be the pinned commit {TPDE_COMMIT}")
    if data.get("primary_target") != "linux-elf-x86-64":
        errors.append("primary_target must be linux-elf-x86-64")
    if data.get("secondary_target") != "aarch64-analyzed-unreleased":
        errors.append("secondary_target must be aarch64-analyzed-unreleased")
    if data.get("classifications") != sorted(CLASSIFICATIONS):
        errors.append("classifications must contain the exact sorted classification enum")
    if data.get("reviewed_source_files") != sorted(MANDATORY_SOURCE_FILES):
        errors.append("reviewed_source_files must contain every mandatory TPDE source")

    capabilities = data.get("capabilities")
    if not isinstance(capabilities, list):
        return errors + ["capabilities must be an array"]

    seen: set[str] = set()
    for index, cap in enumerate(capabilities):
        where = f"capabilities[{index}]"
        if not isinstance(cap, dict):
            errors.append(f"{where} must be an object")
            continue
        missing_fields = sorted(CAPABILITY_FIELDS - set(cap))
        extra_fields = sorted(set(cap) - CAPABILITY_FIELDS)
        if missing_fields:
            errors.append(f"{where} missing fields: " + ", ".join(missing_fields))
        if extra_fields:
            errors.append(f"{where} unrecognized fields: " + ", ".join(extra_fields))
        cap_id = cap.get("id")
        if not _nonempty_string(cap_id):
            errors.append(f"{where}.id must be a non-empty string")
            cap_id = where
        elif cap_id in seen:
            errors.append(f"duplicate capability id: {cap_id}")
        else:
            seen.add(cap_id)
        where = str(cap_id)

        for field in (
            "php_requirement",
            "motivating_contract",
            "existing_behavior",
            "missing_behavior",
            "minimal_api_or_shim",
        ):
            if not _nonempty_string(cap.get(field)):
                errors.append(f"{where}.{field} must be a non-empty string")

        classification = cap.get("classification")
        if classification not in CLASSIFICATIONS:
            errors.append(f"{where}.classification is unsupported: {classification!r}")
        if cap.get("owner") not in OWNERS:
            errors.append(f"{where}.owner is unsupported: {cap.get('owner')!r}")
        if cap.get("upstreamability") not in UPSTREAMABILITY:
            errors.append(
                f"{where}.upstreamability is unsupported: {cap.get('upstreamability')!r}"
            )
        if cap.get("critical") is not True:
            errors.append(f"{where}.critical must be true")

        refs = cap.get("source_refs")
        if not isinstance(refs, list) or not refs:
            errors.append(f"{where}.source_refs must contain evidence")
            refs = []
        for ref_index, ref in enumerate(refs):
            ref_where = f"{where}.source_refs[{ref_index}]"
            if not isinstance(ref, dict):
                errors.append(f"{ref_where} must be an object")
                continue
            missing_fields = sorted(SOURCE_REF_FIELDS - set(ref))
            extra_fields = sorted(set(ref) - SOURCE_REF_FIELDS)
            if missing_fields:
                errors.append(f"{ref_where} missing fields: " + ", ".join(missing_fields))
            if extra_fields:
                errors.append(
                    f"{ref_where} unrecognized fields: " + ", ".join(extra_fields)
                )
            path_value = ref.get("path")
            symbol = ref.get("symbol")
            evidence = ref.get("evidence")
            start = ref.get("start_line")
            end = ref.get("end_line")
            if not _nonempty_string(path_value):
                errors.append(f"{ref_where}.path must be a non-empty string")
                continue
            path = PurePosixPath(path_value)
            if path.is_absolute() or ".." in path.parts:
                errors.append(f"{ref_where}.path must be a safe relative POSIX path")
            if not _nonempty_string(symbol):
                errors.append(f"{ref_where}.symbol must be a non-empty string")
            if not _nonempty_string(evidence):
                errors.append(f"{ref_where}.evidence must be a non-empty string")
            if not isinstance(start, int) or isinstance(start, bool) or start < 1:
                errors.append(f"{ref_where}.start_line must be a positive integer")
            if (
                not isinstance(end, int)
                or isinstance(end, bool)
                or not isinstance(start, int)
                or end < start
            ):
                errors.append(f"{ref_where}.end_line must be at least start_line")
            if tpde is not None and _nonempty_string(path_value):
                source = tpde / path_value
                if not source.is_file():
                    errors.append(f"{ref_where}: source file does not exist: {path_value}")
                    continue
                lines = source.read_text(encoding="utf-8").splitlines()
                if not isinstance(start, int) or not isinstance(end, int):
                    continue
                if end > len(lines):
                    errors.append(
                        f"{ref_where}: line range {start}-{end} exceeds {len(lines)} lines"
                    )
                    continue
                if _nonempty_string(symbol) and symbol not in "\n".join(lines[start - 1 : end]):
                    errors.append(
                        f"{ref_where}: symbol {symbol!r} not found in {path_value}:{start}-{end}"
                    )

        if classification == "covered":
            if cap.get("missing_behavior") != "None.":
                errors.append(f"{where}: covered capability must have missing_behavior 'None.'")
            if cap.get("minimal_api_or_shim") != "None.":
                errors.append(f"{where}: covered capability must not propose an API or shim")
        if classification == "blocked":
            if not _nonempty_string(cap.get("blocker_id")):
                errors.append(f"{where}: blocked capability requires blocker_id")
            if not _nonempty_string(cap.get("decision_id")):
                errors.append(f"{where}: blocked capability requires decision_id")
            if cap.get("critical") is True:
                errors.append(f"{where}: critical capability remains unresolved (blocked)")
        elif cap.get("blocker_id") is not None or cap.get("decision_id") is not None:
            errors.append(f"{where}: blocker_id and decision_id must be null unless blocked")

        target_impact = cap.get("target_impact")
        if not isinstance(target_impact, dict):
            errors.append(f"{where}.target_impact must be an object")
        else:
            if set(target_impact) != {"x86_64", "aarch64"}:
                errors.append(f"{where}.target_impact must contain only x86_64 and aarch64")
            for target, impact in target_impact.items():
                target_where = f"{where}.target_impact.{target}"
                if not isinstance(impact, dict):
                    errors.append(f"{target_where} must be an object")
                    continue
                missing_fields = sorted(TARGET_FIELDS - set(impact))
                extra_fields = sorted(set(impact) - TARGET_FIELDS)
                if missing_fields:
                    errors.append(
                        f"{target_where} missing fields: " + ", ".join(missing_fields)
                    )
                if extra_fields:
                    errors.append(
                        f"{target_where} unrecognized fields: " + ", ".join(extra_fields)
                    )
                if impact.get("status") not in TARGET_STATUSES:
                    errors.append(
                        f"{target_where}.status is unsupported: {impact.get('status')!r}"
                    )
                if target == "aarch64" and impact.get("status") == "required":
                    errors.append(f"{target_where}: AArch64 is analyzed but not released")
                if not _nonempty_string(impact.get("impact")):
                    errors.append(f"{target_where}.impact must be a non-empty string")

        tests = cap.get("required_tests")
        if not isinstance(tests, list) or not tests or not all(_nonempty_string(test) for test in tests):
            errors.append(f"{where}.required_tests must contain non-empty test descriptions")

    missing_ids = sorted(CRITICAL_IDS - seen)
    extra_ids = sorted(seen - CRITICAL_IDS)
    if missing_ids:
        errors.append("missing critical capability ids: " + ", ".join(missing_ids))
    if extra_ids:
        errors.append("unrecognized capability ids: " + ", ".join(extra_ids))

    if tpde is not None:
        head = _run_git(tpde, "rev-parse", "HEAD")
        if head.returncode != 0:
            errors.append("cannot read TPDE git HEAD: " + head.stderr.strip())
        elif head.stdout.strip() != TPDE_COMMIT:
            errors.append(
                f"TPDE checkout HEAD is {head.stdout.strip()}, expected {TPDE_COMMIT}"
            )
        status = _run_git(tpde, "status", "--short")
        if status.returncode != 0:
            errors.append("cannot read TPDE git status: " + status.stderr.strip())
        elif status.stdout.strip():
            errors.append("TPDE checkout is not clean")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tpde", required=True, type=Path, help="pinned TPDE checkout")
    parser.add_argument("--check", action="store_true", help="validate without modifying files")
    args = parser.parse_args()
    if not args.check:
        parser.error("--check is required; this validator never rewrites the inventory")

    root = Path(__file__).resolve().parents[3]
    manifest = root / "docs/native-engine/tpde/required-capabilities.json"
    try:
        data = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"error: cannot load {manifest}: {exc}", file=sys.stderr)
        return 1

    errors = validate_document(data, args.tpde.resolve())
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1
    print(f"validated {len(data['capabilities'])} TPDE capabilities at {TPDE_COMMIT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
