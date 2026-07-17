#!/usr/bin/env python3
"""Run deterministic call-1-through-10 PHP benchmarks with provenance."""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

THIS_DIR = Path(__file__).resolve().parent
REPOSITORY_ROOT = THIS_DIR.parents[1]
TEST_TOOLS = REPOSITORY_ROOT / "tests" / "native"
sys.path.insert(0, str(TEST_TOOLS))

from capture_manifest import capture_manifest  # noqa: E402
from harness_lib import canonical_executable, json_load, relative_artifact_path, stable_json_dump, utc_now  # noqa: E402
from schema_validation import validate_benchmark_result  # noqa: E402


BASELINE_ID = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
COMMIT_ID = re.compile(r"^[0-9a-f]{40,64}$")


def discover_scenarios(value: Path) -> Tuple[Path, List[Path]]:
    resolved = value.resolve()
    if resolved.is_file():
        return resolved.parent, [resolved]
    if not resolved.is_dir():
        raise ValueError("scenario path is neither a file nor directory: {}".format(value))
    scenarios = sorted(
        (path for path in resolved.glob("*.json") if path.is_file()),
        key=lambda path: path.name.encode("utf-8"),
    )
    if not scenarios:
        raise ValueError("scenario directory contains no JSON descriptors: {}".format(value))
    return resolved, scenarios


def validate_descriptor(path: Path) -> Dict[str, Any]:
    descriptor = json_load(path)
    if not isinstance(descriptor, dict):
        raise ValueError("{}: descriptor must be an object".format(path))
    required = {"schema_version", "scenario_id", "title", "php_file", "warmup_calls", "config", "expected_checksum"}
    missing = sorted(required - set(descriptor))
    if missing:
        raise ValueError("{}: descriptor missing {}".format(path, ", ".join(missing)))
    if descriptor["schema_version"] != 1:
        raise ValueError("{}: unsupported descriptor schema".format(path))
    if not isinstance(descriptor["scenario_id"], str) or not descriptor["scenario_id"]:
        raise ValueError("{}: invalid scenario_id".format(path))
    if not isinstance(descriptor["warmup_calls"], int) or isinstance(descriptor["warmup_calls"], bool) or descriptor["warmup_calls"] < 0:
        raise ValueError("{}: warmup_calls must be a non-negative integer".format(path))
    if not isinstance(descriptor["config"], dict):
        raise ValueError("{}: config must be an object".format(path))
    if not re.match(r"^[0-9a-f]{64}$", descriptor["expected_checksum"]):
        raise ValueError("{}: expected_checksum must be SHA-256".format(path))
    php_file = (path.parent / descriptor["php_file"]).resolve()
    try:
        php_file.relative_to(path.parent.resolve())
    except ValueError:
        raise ValueError("{}: php_file escapes descriptor directory".format(path))
    if not php_file.is_file():
        raise ValueError("{}: php_file does not exist".format(path))
    return descriptor


def percentile(values: Sequence[int], quantile: float) -> float:
    ordered = sorted(values)
    if len(ordered) == 1:
        return float(ordered[0])
    position = (len(ordered) - 1) * quantile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return float(ordered[lower])
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def distribution(values: Sequence[int]) -> Dict[str, Any]:
    if not values:
        return {"count": 0, "max": None, "median": None, "min": None, "p90": None, "p95": None}
    return {
        "count": len(values),
        "max": max(values),
        "median": statistics.median(values),
        "min": min(values),
        "p90": percentile(values, 0.90),
        "p95": percentile(values, 0.95),
    }


def elf_text_size(binary: Path) -> Dict[str, Any]:
    size_tool = shutil.which("size")
    if size_tool is None:
        return {"bytes": None, "source": None, "unsupported_reason": "size tool is unavailable"}
    try:
        completed = subprocess.run(
            [size_tool, "-A", str(binary)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"bytes": None, "source": None, "unsupported_reason": "size -A failed: {}".format(error)}
    if completed.returncode != 0:
        return {"bytes": None, "source": None, "unsupported_reason": "size -A returned {}".format(completed.returncode)}
    for line in completed.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0] == ".text":
            try:
                return {"bytes": int(fields[1], 0), "source": "{} -A".format(size_tool), "unsupported_reason": None}
            except ValueError:
                break
    return {"bytes": None, "source": "{} -A".format(size_tool), "unsupported_reason": "output contained no parseable .text section"}


def validate_promotion_manifest(manifest: Dict[str, Any], expected_commit: Optional[str]) -> None:
    if expected_commit is None or not COMMIT_ID.match(expected_commit):
        raise ValueError("baseline promotion requires --expected-commit with a full Git object ID")
    provenance = manifest["provenance"]
    if provenance["commit"] is None:
        raise ValueError("baseline promotion refused: binary has unknown repository commit ({})".format(provenance["reason"]))
    if provenance["dirty"] is not False:
        raise ValueError("baseline promotion refused: binary source worktree is dirty or its state is unknown")
    if provenance["commit"] != expected_commit:
        raise ValueError(
            "baseline promotion refused: binary commit {} does not match expected {}".format(provenance["commit"], expected_commit)
        )


def prepare_destination(output_dir: Optional[Path], baseline_id: Optional[str], manifest: Dict[str, Any], expected_commit: Optional[str]) -> Tuple[Path, str]:
    if baseline_id is not None:
        if output_dir is not None:
            raise ValueError("--output-dir and --promote-baseline are mutually exclusive")
        if not BASELINE_ID.match(baseline_id):
            raise ValueError("baseline ID must match {}".format(BASELINE_ID.pattern))
        validate_promotion_manifest(manifest, expected_commit)
        destination = THIS_DIR / "baselines" / baseline_id
        if destination.exists():
            raise ValueError("baseline destination already exists: {}".format(destination))
        return destination, "canonical_baseline"
    if output_dir is None:
        raise ValueError("local runs require --output-dir")
    return output_dir.resolve(), "local_artifact"


def run_probe(command: Sequence[str], timeout: float, artifact_prefix: Path) -> Tuple[Dict[str, Any], bytes, bytes]:
    probe_json = artifact_prefix.with_name(artifact_prefix.name + ".process.json")
    stdout_path = artifact_prefix.with_name(artifact_prefix.name + ".stdout.bin")
    stderr_path = artifact_prefix.with_name(artifact_prefix.name + ".stderr.bin")
    probe_command = [
        sys.executable,
        str(THIS_DIR / "process_probe.py"),
        "--json-out",
        str(probe_json),
        "--stdout-out",
        str(stdout_path),
        "--stderr-out",
        str(stderr_path),
        "--timeout",
        str(timeout),
        "--",
    ] + list(command)
    completed = subprocess.run(probe_command, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout + 10)
    if completed.returncode != 0:
        raise RuntimeError("resource probe failed: {}".format(completed.stderr.decode("utf-8", errors="replace")))
    return json_load(probe_json), stdout_path.read_bytes(), stderr_path.read_bytes()


def run_scenario(
    binary: Path,
    descriptor_path: Path,
    descriptor: Dict[str, Any],
    repetitions: int,
    steady_state_calls: int,
    timeout: float,
    artifacts: Path,
    result_parent: Path,
    code_size: Dict[str, Any],
) -> Dict[str, Any]:
    samples = []
    for repetition in range(1, repetitions + 1):
        prefix = artifacts / "{}-{:03d}".format(descriptor["scenario_id"], repetition)
        process, stdout, stderr = run_probe(
            [str(binary), str(THIS_DIR / "scenario_driver.php"), "--descriptor", str(descriptor_path), str(steady_state_calls)],
            timeout,
            prefix,
        )
        if process["termination"]["kind"] != "exit" or process["termination"]["exit_code"] != 0:
            raise RuntimeError(
                "scenario {} repetition {} failed with {}: {}".format(
                    descriptor["scenario_id"], repetition, process["termination"], stderr.decode("utf-8", errors="replace")
                )
            )
        try:
            driver = json.loads(stdout.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise RuntimeError("scenario {} returned invalid UTF-8 JSON: {}".format(descriptor["scenario_id"], error))
        if driver.get("protocol_version") != 1 or driver.get("scenario_id") != descriptor["scenario_id"]:
            raise RuntimeError("scenario driver protocol or scenario ID mismatch")
        calls = driver.get("call_ns")
        if not isinstance(calls, list) or len(calls) != 10 or any(not isinstance(item, int) or item < 0 for item in calls):
            raise RuntimeError("scenario driver did not return exactly ten valid call timings")
        sample = {
            "artifacts": {
                "process": relative_artifact_path(prefix.with_name(prefix.name + ".process.json"), result_parent),
                "stderr": relative_artifact_path(prefix.with_name(prefix.name + ".stderr.bin"), result_parent),
                "stdout": relative_artifact_path(prefix.with_name(prefix.name + ".stdout.bin"), result_parent),
            },
            "call_ns": calls,
            "compile_phase": driver["compile_phase"],
            "compile_phase_unsupported_reason": driver["compile_phase_unsupported_reason"],
            "opcache": driver["opcache"],
            "process": {
                "max_rss_bytes": process["max_rss_bytes"],
                "metric_scope": "outer PHP process from spawn until termination",
                "metric_sources": process["metric_sources"],
                "system_ns": process["system_ns"],
                "user_ns": process["user_ns"],
                "wall_ns": process["wall_ns"],
            },
            "repetition": repetition,
            "result_checksum": driver["result_checksum"],
            "steady_state_ns": driver["steady_state_ns"],
        }
        samples.append(sample)
    all_calls = [call for sample in samples for call in sample["call_ns"]]
    per_call = [distribution([sample["call_ns"][index] for sample in samples]) for index in range(10)]
    return {
        "aggregate": {
            "call_ns": distribution(all_calls),
            "per_call_index_ns": per_call,
            "process_max_rss_bytes": distribution([sample["process"]["max_rss_bytes"] for sample in samples]),
            "process_system_ns": distribution([sample["process"]["system_ns"] for sample in samples]),
            "process_user_ns": distribution([sample["process"]["user_ns"] for sample in samples]),
            "process_wall_ns": distribution([sample["process"]["wall_ns"] for sample in samples]),
            "steady_state_ns": distribution([item for sample in samples for item in sample["steady_state_ns"]]),
        },
        "code_size": code_size,
        "descriptor": descriptor_path.name,
        "native_code_size": {
            "bytes": None,
            "source": None,
            "unsupported_reason": "W00 has no native engine code-size source",
        },
        "samples": samples,
        "scenario_id": descriptor["scenario_id"],
        "title": descriptor["title"],
        "warmup": {"calls": descriptor["warmup_calls"], "config": descriptor["config"]},
    }


def build_result(
    args: argparse.Namespace,
    binary: Path,
    manifest: Dict[str, Any],
    destination: Path,
    run_mode: str,
) -> Path:
    scenario_root, scenario_paths = discover_scenarios(args.scenario)
    descriptors = [validate_descriptor(path) for path in scenario_paths]
    ids = [descriptor["scenario_id"] for descriptor in descriptors]
    if len(ids) != len(set(ids)):
        raise ValueError("scenario IDs must be unique")
    destination.mkdir(parents=True, exist_ok=True)
    artifacts = destination / "artifacts"
    artifacts.mkdir(parents=True, exist_ok=True)
    result_path = destination / "benchmark-result.json"
    code_size = {
        "binary_bytes": binary.stat().st_size,
        "binary_source": "stat.st_size",
        "elf_text": elf_text_size(binary),
    }
    scenarios = [
        run_scenario(
            binary,
            path,
            descriptor,
            args.repetitions,
            args.steady_state_calls,
            args.timeout,
            artifacts,
            result_path.parent,
            code_size,
        )
        for path, descriptor in zip(scenario_paths, descriptors)
    ]
    document = {
        "result_type": "benchmark_result",
        "run_manifest": {
            "binary": manifest,
            "configuration": {
                "repetitions": args.repetitions,
                "run_mode": run_mode,
                "scenario_count": len(scenarios),
                "steady_state_calls": args.steady_state_calls,
                "timeout_seconds": args.timeout,
            },
            "generated_at_utc": utc_now(),
            "host": manifest["host"],
        },
        "scenarios": scenarios,
        "schema_version": 1,
        "summary": {
            "failed": 0,
            "samples": len(scenarios) * args.repetitions,
            "scenarios": len(scenarios),
            "status": "pass",
        },
        "volatile_fields": [
            "/run_manifest/generated_at_utc",
            "/run_manifest/binary/generated_at_utc",
            "/scenarios/*/samples/*/call_ns",
            "/scenarios/*/samples/*/process",
            "/scenarios/*/samples/*/steady_state_ns",
            "/scenarios/*/aggregate",
        ],
    }
    validate_benchmark_result(document)
    stable_json_dump(manifest, destination / "binary-manifest.json")
    stable_json_dump(document, result_path)
    return result_path


def run(args: argparse.Namespace) -> Path:
    binary = canonical_executable(args.php)
    manifest = capture_manifest(str(binary))
    destination, run_mode = prepare_destination(args.output_dir, args.promote_baseline, manifest, args.expected_commit)
    if run_mode != "canonical_baseline":
        return build_result(args, binary, manifest, destination, run_mode)

    baselines = destination.parent
    baselines.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".staging-{}-".format(args.promote_baseline), dir=str(baselines)))
    try:
        staged_result = build_result(args, binary, manifest, staging, run_mode)
        os.replace(str(staging), str(destination))
        return destination / staged_result.name
    except BaseException:
        shutil.rmtree(str(staging), ignore_errors=True)
        raise


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--php", required=True, help="explicit PHP executable path")
    parser.add_argument("--scenario", required=True, type=Path, help="descriptor file or directory")
    parser.add_argument("--output-dir", type=Path, help="local, non-versioned artifact directory")
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--steady-state-calls", type=int, default=0)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--promote-baseline", metavar="BASELINE_ID")
    parser.add_argument("--expected-commit", help="required full commit ID for baseline promotion")
    args = parser.parse_args()
    if args.repetitions < 1:
        parser.error("--repetitions must be positive")
    if args.steady_state_calls < 0:
        parser.error("--steady-state-calls must be non-negative")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    return args


def main() -> int:
    args = parse_args()
    try:
        result = run(args)
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        print("benchmark_runner.py: {}".format(error), file=sys.stderr)
        return 2
    print(result)
    return 0


if __name__ == "__main__":
    sys.exit(main())
