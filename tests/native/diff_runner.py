#!/usr/bin/env python3
"""Byte-exact PHP differential runner with raw-output artifacts."""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path
from typing import Any, Dict, List, Tuple

from harness_lib import (
    EXIT_ALL_EQUIVALENT,
    EXIT_DIFFERENT,
    EXIT_HARNESS_ERROR,
    EXIT_TIMEOUT,
    FileProcessResult,
    canonical_executable,
    files_equal,
    raw_file_json,
    relative_artifact_path,
    run_process_to_files,
    stable_json_dump,
    utc_now,
)
from schema_validation import validate_diff_result


def discover_cases(case_path: Path) -> Tuple[Path, List[Path]]:
    resolved = case_path.resolve()
    if resolved.is_file():
        return resolved.parent, [resolved]
    if not resolved.is_dir():
        raise ValueError("case path is neither a file nor directory: {}".format(case_path))
    cases = sorted(
        (path for path in resolved.rglob("*.php") if path.is_file()),
        key=lambda path: path.relative_to(resolved).as_posix().encode("utf-8"),
    )
    if not cases:
        raise ValueError("case directory contains no .php files: {}".format(case_path))
    return resolved, cases


def artifact_component(case_id: str) -> str:
    readable = "".join(character if character.isalnum() or character in "._-" else "_" for character in case_id)
    return "{}-{}".format(readable[:80], hashlib.sha256(case_id.encode("utf-8")).hexdigest()[:12])


def process_json(result: FileProcessResult, json_parent: Path) -> Dict[str, Any]:
    stdout = raw_file_json(result.stdout_path)
    stderr = raw_file_json(result.stderr_path)
    stdout["artifact"] = relative_artifact_path(result.stdout_path, json_parent)
    stderr["artifact"] = relative_artifact_path(result.stderr_path, json_parent)
    return {
        "duration_ns": result.duration_ns,
        "stderr": stderr,
        "stdout": stdout,
        "termination": result.termination_json(),
    }


def compare_case(
    reference: Path,
    candidate: Path,
    case: Path,
    case_id: str,
    timeout: float,
    artifacts: Path,
    json_parent: Path,
) -> Dict[str, Any]:
    component = artifact_component(case_id)
    reference_prefix = artifacts / (component + ".reference")
    candidate_prefix = artifacts / (component + ".candidate")
    reference_result = run_process_to_files(
        [str(reference), str(case)],
        timeout,
        reference_prefix.with_name(reference_prefix.name + ".stdout.bin"),
        reference_prefix.with_name(reference_prefix.name + ".stderr.bin"),
        cwd=case.parent,
    )
    candidate_result = run_process_to_files(
        [str(candidate), str(case)],
        timeout,
        candidate_prefix.with_name(candidate_prefix.name + ".stdout.bin"),
        candidate_prefix.with_name(candidate_prefix.name + ".stderr.bin"),
        cwd=case.parent,
    )
    differences = []
    if not files_equal(reference_result.stdout_path, candidate_result.stdout_path):
        differences.append("stdout")
    if not files_equal(reference_result.stderr_path, candidate_result.stderr_path):
        differences.append("stderr")
    if reference_result.termination_json() != candidate_result.termination_json():
        differences.append("termination")
    if reference_result.timed_out or candidate_result.timed_out:
        status = "TIMEOUT"
    elif differences:
        status = "DIFFERENT"
    else:
        status = "EQUIVALENT"
    return {
        "candidate": process_json(candidate_result, json_parent),
        "case_id": case_id,
        "differences": differences,
        "fixture": case_id,
        "reference": process_json(reference_result, json_parent),
        "status": status,
    }


def run(reference_value: str, candidate_value: str, case_value: Path, timeout: float, json_out: Path) -> Tuple[Dict[str, Any], int]:
    reference = canonical_executable(reference_value)
    candidate = canonical_executable(candidate_value)
    case_root, cases = discover_cases(case_value)
    json_out = json_out.resolve()
    artifacts = json_out.parent / (json_out.stem + ".artifacts")
    if artifacts.exists() and not artifacts.is_dir():
        raise ValueError("artifact path exists and is not a directory: {}".format(artifacts))
    artifacts.mkdir(parents=True, exist_ok=True)
    case_results = []
    for case in cases:
        case_id = case.relative_to(case_root).as_posix()
        case_results.append(compare_case(reference, candidate, case, case_id, timeout, artifacts, json_out.parent))
    counts = {"different": 0, "equivalent": 0, "harness_error": 0, "timeout": 0, "total": len(case_results)}
    for item in case_results:
        counts[item["status"].lower()] += 1
    if counts["timeout"]:
        overall, exit_code = "timeout", EXIT_TIMEOUT
    elif counts["different"]:
        overall, exit_code = "different", EXIT_DIFFERENT
    else:
        overall, exit_code = "equivalent", EXIT_ALL_EQUIVALENT
    document = {
        "binaries": {"candidate": str(candidate), "reference": str(reference)},
        "cases": case_results,
        "comparison": {
            "normalization": {"enabled": False, "rules": []},
            "outputs": "byte_exact",
            "termination_fields": ["exit_code", "signal", "timeout"],
        },
        "exit_code": exit_code,
        "generated_at_utc": utc_now(),
        "overall_status": overall,
        "result_type": "differential_result",
        "schema_version": 1,
        "summary": counts,
        "timeout_seconds": timeout,
        "volatile_fields": [
            "/generated_at_utc",
            "/cases/*/candidate/duration_ns",
            "/cases/*/reference/duration_ns",
            "/cases/*/candidate/stdout/artifact",
            "/cases/*/candidate/stderr/artifact",
            "/cases/*/reference/stdout/artifact",
            "/cases/*/reference/stderr/artifact",
        ],
    }
    validate_diff_result(document)
    stable_json_dump(document, json_out)
    return document, exit_code


def error_document(message: str) -> Dict[str, Any]:
    return {
        "cases": [],
        "error": message,
        "exit_code": EXIT_HARNESS_ERROR,
        "generated_at_utc": utc_now(),
        "overall_status": "harness_error",
        "result_type": "differential_result",
        "schema_version": 1,
        "summary": {"different": 0, "equivalent": 0, "harness_error": 1, "timeout": 0, "total": 0},
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", required=True, help="explicit reference PHP path")
    parser.add_argument("--candidate", required=True, help="explicit candidate PHP path")
    parser.add_argument("--case", required=True, type=Path, help="PHP file or recursively scanned directory")
    parser.add_argument("--timeout", required=True, type=float)
    parser.add_argument("--json-out", required=True, type=Path)
    args = parser.parse_args()
    try:
        _, exit_code = run(args.reference, args.candidate, args.case, args.timeout, args.json_out)
    except (OSError, ValueError) as error:
        document = error_document(str(error))
        try:
            stable_json_dump(document, args.json_out.resolve())
        except OSError:
            pass
        print("diff_runner.py: {}".format(error), file=sys.stderr)
        return EXIT_HARNESS_ERROR
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
