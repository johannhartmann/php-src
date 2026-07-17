#!/usr/bin/env python3
"""Small, strict validators used without third-party JSON Schema packages."""

from __future__ import annotations

import re
from typing import Any, Dict, Iterable, List


HEX_64 = re.compile(r"^[0-9a-f]{64}$")
COMMIT = re.compile(r"^[0-9a-f]{40,64}$")
RESULT_TYPES = {"binary_manifest", "differential_result", "benchmark_result"}


class ValidationError(ValueError):
    pass


def require_object(value: Any, where: str) -> Dict[str, Any]:
    if not isinstance(value, dict):
        raise ValidationError("{} must be an object".format(where))
    return value


def require_list(value: Any, where: str) -> List[Any]:
    if not isinstance(value, list):
        raise ValidationError("{} must be an array".format(where))
    return value


def require_keys(value: Dict[str, Any], keys: Iterable[str], where: str) -> None:
    missing = sorted(set(keys) - set(value))
    if missing:
        raise ValidationError("{} missing keys: {}".format(where, ", ".join(missing)))


def require_type(value: Any, expected: type, where: str) -> None:
    if expected is int and isinstance(value, bool):
        raise ValidationError("{} must be int".format(where))
    if not isinstance(value, expected):
        raise ValidationError("{} must be {}".format(where, expected.__name__))


def validate_raw_bytes(value: Any, where: str) -> None:
    obj = require_object(value, where)
    require_keys(obj, ["length", "sha256"], where)
    require_type(obj["length"], int, where + ".length")
    require_type(obj["sha256"], str, where + ".sha256")
    if obj["length"] < 0 or not HEX_64.match(obj["sha256"]):
        raise ValidationError("{} has invalid length or sha256".format(where))


def validate_termination(value: Any, where: str) -> None:
    obj = require_object(value, where)
    require_keys(obj, ["exit_code", "kind", "signal", "timeout"], where)
    if obj["kind"] not in ("exit", "signal", "timeout"):
        raise ValidationError("{} has invalid kind".format(where))
    require_type(obj["timeout"], bool, where + ".timeout")
    if obj["kind"] == "exit" and not isinstance(obj["exit_code"], int):
        raise ValidationError("{} exit requires exit_code".format(where))
    if obj["kind"] == "signal" and not isinstance(obj["signal"], int):
        raise ValidationError("{} signal requires signal number".format(where))
    if obj["kind"] == "timeout" and not obj["timeout"]:
        raise ValidationError("{} timeout must be true".format(where))


def validate_manifest(value: Any) -> None:
    obj = require_object(value, "manifest")
    require_keys(obj, ["schema_version", "result_type", "binary", "host", "php", "provenance"], "manifest")
    if obj["schema_version"] != 1 or obj["result_type"] != "binary_manifest":
        raise ValidationError("unsupported manifest contract")
    binary = require_object(obj["binary"], "manifest.binary")
    require_keys(binary, ["canonical_path", "elf_build_id", "sha256", "size_bytes"], "manifest.binary")
    if not HEX_64.match(binary["sha256"]):
        raise ValidationError("manifest.binary.sha256 is invalid")
    require_type(binary["size_bytes"], int, "manifest.binary.size_bytes")
    php = require_object(obj["php"], "manifest.php")
    require_keys(php, ["facts", "info", "version_command"], "manifest.php")
    require_object(php["info"], "manifest.php.info")
    validate_raw_bytes(php["version_command"]["stdout"], "manifest.php.version_command.stdout")
    validate_raw_bytes(php["version_command"]["stderr"], "manifest.php.version_command.stderr")
    provenance = require_object(obj["provenance"], "manifest.provenance")
    require_keys(provenance, ["commit", "dirty", "reason", "repository_root"], "manifest.provenance")
    if provenance["commit"] is not None and not COMMIT.match(provenance["commit"]):
        raise ValidationError("manifest.provenance.commit is invalid")


def validate_diff_result(value: Any) -> None:
    obj = require_object(value, "differential result")
    require_keys(obj, ["schema_version", "result_type", "overall_status", "exit_code", "cases", "summary"], "differential result")
    if obj["schema_version"] != 1 or obj["result_type"] != "differential_result":
        raise ValidationError("unsupported differential result contract")
    if obj["overall_status"] not in ("equivalent", "different", "timeout", "harness_error"):
        raise ValidationError("invalid overall_status")
    require_type(obj["exit_code"], int, "differential result.exit_code")
    case_ids = set()
    for index, case in enumerate(require_list(obj["cases"], "differential result.cases")):
        where = "differential result.cases[{}]".format(index)
        item = require_object(case, where)
        require_keys(item, ["case_id", "fixture", "status", "reference", "candidate", "differences"], where)
        require_type(item["case_id"], str, where + ".case_id")
        if item["case_id"] in case_ids:
            raise ValidationError("duplicate case_id: {}".format(item["case_id"]))
        case_ids.add(item["case_id"])
        if item["status"] not in ("EQUIVALENT", "DIFFERENT", "TIMEOUT", "HARNESS_ERROR"):
            raise ValidationError("{} has invalid status".format(where))
        for side in ("reference", "candidate"):
            process = require_object(item[side], where + "." + side)
            require_keys(process, ["duration_ns", "stdout", "stderr", "termination"], where + "." + side)
            validate_raw_bytes(process["stdout"], where + "." + side + ".stdout")
            validate_raw_bytes(process["stderr"], where + "." + side + ".stderr")
            validate_termination(process["termination"], where + "." + side + ".termination")


def validate_benchmark_result(value: Any) -> None:
    obj = require_object(value, "benchmark result")
    require_keys(obj, ["schema_version", "result_type", "run_manifest", "scenarios", "summary"], "benchmark result")
    if obj["schema_version"] != 1 or obj["result_type"] != "benchmark_result":
        raise ValidationError("unsupported benchmark result contract")
    run_manifest = require_object(obj["run_manifest"], "benchmark result.run_manifest")
    require_keys(run_manifest, ["binary", "host", "configuration", "generated_at_utc"], "benchmark result.run_manifest")
    validate_manifest(run_manifest["binary"])
    scenario_ids = set()
    for index, scenario in enumerate(require_list(obj["scenarios"], "benchmark result.scenarios")):
        where = "benchmark result.scenarios[{}]".format(index)
        item = require_object(scenario, where)
        require_keys(item, ["scenario_id", "warmup", "samples", "aggregate", "native_code_size"], where)
        if item["scenario_id"] in scenario_ids:
            raise ValidationError("duplicate benchmark scenario_id")
        scenario_ids.add(item["scenario_id"])
        native_size = require_object(item["native_code_size"], where + ".native_code_size")
        if native_size.get("bytes") is not None or not native_size.get("unsupported_reason"):
            raise ValidationError("{} native_code_size must be explicitly unsupported in W00".format(where))
        samples = require_list(item["samples"], where + ".samples")
        if not samples:
            raise ValidationError("{} needs at least one sample".format(where))
        for sample_index, sample in enumerate(samples):
            sample_where = where + ".samples[{}]".format(sample_index)
            sample_obj = require_object(sample, sample_where)
            require_keys(sample_obj, ["call_ns", "result_checksum", "process", "compile_phase", "steady_state_ns"], sample_where)
            calls = require_list(sample_obj["call_ns"], sample_where + ".call_ns")
            if len(calls) != 10 or any(not isinstance(call, int) or isinstance(call, bool) or call < 0 for call in calls):
                raise ValidationError("{} call_ns must contain ten non-negative integers".format(sample_where))
            if sample_obj["compile_phase"] is not None:
                raise ValidationError("{} compile_phase must be null unless explicitly measured".format(sample_where))


def validate_document(value: Any) -> None:
    obj = require_object(value, "document")
    result_type = obj.get("result_type")
    if result_type == "binary_manifest":
        validate_manifest(obj)
    elif result_type == "differential_result":
        validate_diff_result(obj)
    elif result_type == "benchmark_result":
        validate_benchmark_result(obj)
    else:
        raise ValidationError("unknown result_type: {!r}".format(result_type))
