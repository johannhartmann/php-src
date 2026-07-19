#!/usr/bin/env python3
"""Validate, record, aggregate, and render native-engine wave results.

The checked-in JSON Schemas are the contract for external tooling.  This CLI
intentionally implements only the explicit structural checks used by that
contract; it is not a general JSON Schema implementation.
"""

import argparse
import datetime as dt
import json
import os
from pathlib import Path, PureWindowsPath
import re
import sys
import tempfile
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple


EXIT_OK = 0
EXIT_GATE_FAIL = 1
EXIT_SCHEMA_INVALID = 2
EXIT_USAGE = 64

FORMAT_VERSION = "1.0.0"
STATUSES = {"pending", "running", "pass", "fail", "blocked", "skipped"}
SUMMARY_STATUSES = STATUSES | {"missing"}
COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
WAVE_RE = re.compile(r"^W(?:0[0-9]|1[0-8])$")
TASK_RE = re.compile(r"^W(?:0[0-9]|1[0-8])-[A-Za-z0-9][A-Za-z0-9-]*$")
REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DEFINITION = REPO_ROOT / "docs/native-engine/waves/waves.json"
DEFAULT_LEDGER = REPO_ROOT / "docs/native-engine/waves/ledger.json"
LEDGER_STATES = {
    "unsealed", "revalidated", "sealed", "invalid", "pending", "unstarted",
}
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


class UsageParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        self.print_usage(sys.stderr)
        self.exit(EXIT_USAGE, "%s: error: %s\n" % (self.prog, message))


class ValidationError(Exception):
    def __init__(self, kind: str, issues: Sequence[str]):
        super().__init__("; ".join(issues))
        self.kind = kind
        self.issues = list(issues)


def read_json(path: Path) -> Any:
    try:
        with path.open("r", encoding="utf-8") as handle:
            return json.load(handle)
    except (OSError, json.JSONDecodeError) as exc:
        raise ValidationError("JSON", ["%s: %s" % (path, exc)]) from exc


def write_json_atomic(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(
        prefix=".%s." % path.name, suffix=".tmp", dir=str(path.parent)
    )
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(value, handle, indent=2, sort_keys=True)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def write_text_atomic(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(
        prefix=".%s." % path.name, suffix=".tmp", dir=str(path.parent)
    )
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(value)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def is_string(value: Any) -> bool:
    return isinstance(value, str)


def is_boolean(value: Any) -> bool:
    return isinstance(value, bool)


def is_integer(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def require_object(value: Any, path: str, issues: List[str]) -> Optional[Dict[str, Any]]:
    if not isinstance(value, dict):
        issues.append("%s must be an object" % path)
        return None
    return value


def require_array(value: Any, path: str, issues: List[str]) -> Optional[List[Any]]:
    if not isinstance(value, list):
        issues.append("%s must be an array" % path)
        return None
    return value


def require_keys(value: Dict[str, Any], keys: Iterable[str], path: str, issues: List[str]) -> None:
    for key in keys:
        if key not in value:
            issues.append("%s.%s is required" % (path, key))


def require_string(value: Dict[str, Any], key: str, path: str, issues: List[str], nonempty: bool = True) -> None:
    if key not in value:
        return
    item = value[key]
    if not is_string(item) or (nonempty and not item.strip()):
        issues.append("%s.%s must be a non-empty string" % (path, key))


def require_string_array(value: Dict[str, Any], key: str, path: str, issues: List[str]) -> None:
    if key not in value:
        return
    items = require_array(value[key], "%s.%s" % (path, key), issues)
    if items is not None:
        for index, item in enumerate(items):
            if not is_string(item) or not item.strip():
                issues.append("%s.%s[%d] must be a non-empty string" % (path, key, index))


def check_format_version(value: Dict[str, Any], path: str, issues: List[str]) -> None:
    if value.get("format_version") != FORMAT_VERSION:
        issues.append("%s.format_version must equal %s" % (path, FORMAT_VERSION))


def check_commit(value: Any, path: str, issues: List[str], nullable: bool = False) -> None:
    if value is None and nullable:
        return
    if not is_string(value) or not COMMIT_RE.fullmatch(value):
        issues.append("%s must be a 40-character lowercase hexadecimal commit" % path)


def check_relative_path(value: Any, path: str, issues: List[str]) -> None:
    if not is_string(value) or not value:
        issues.append("%s must be a non-empty relative path" % path)
        return
    candidate = Path(value)
    windows_candidate = PureWindowsPath(value)
    if candidate.is_absolute() or windows_candidate.is_absolute() or ".." in candidate.parts or ".." in windows_candidate.parts:
        issues.append("%s must not be absolute or traverse parents" % path)


def check_timestamp(value: Any, path: str, issues: List[str]) -> None:
    if not is_string(value) or not value:
        issues.append("%s must be an RFC 3339 timestamp" % path)
        return
    try:
        parsed = value[:-1] + "+00:00" if value.endswith("Z") else value
        dt.datetime.fromisoformat(parsed)
    except ValueError:
        issues.append("%s must be an RFC 3339 timestamp" % path)


def validate_artifact(value: Any, path: str, issues: List[str]) -> None:
    obj = require_object(value, path, issues)
    if obj is None:
        return
    require_keys(obj, ("kind", "reference"), path, issues)
    if obj.get("kind") not in {"local", "ci"}:
        issues.append("%s.kind must be local or ci" % path)
    require_string(obj, "reference", path, issues)
    if obj.get("kind") == "local" and is_string(obj.get("reference")):
        check_relative_path(obj["reference"], "%s.reference" % path, issues)


def validate_seal_subject(value: Any, path: str, issues: List[str]) -> None:
    obj = require_object(value, path, issues)
    if obj is None:
        return
    require_keys(obj, ("receipt_path", "receipt_sha256"), path, issues)
    unknown = set(obj) - {"receipt_path", "receipt_sha256"}
    if unknown:
        issues.append("%s has unknown fields: %s" % (path, ", ".join(sorted(unknown))))
    if "receipt_path" in obj:
        check_relative_path(obj["receipt_path"], "%s.receipt_path" % path, issues)
    digest = obj.get("receipt_sha256")
    if not is_string(digest) or SHA256_RE.fullmatch(digest) is None:
        issues.append("%s.receipt_sha256 must be a lowercase SHA-256" % path)


def validate_gate_evidence(value: Any) -> List[str]:
    issues: List[str] = []
    obj = require_object(value, "$", issues)
    if obj is None:
        return issues
    required = ("format_version", "wave_id", "gate_id", "status", "summary", "artifact")
    require_keys(obj, required, "$", issues)
    check_format_version(obj, "$", issues)
    if not is_string(obj.get("wave_id")) or not WAVE_RE.fullmatch(obj.get("wave_id", "")):
        issues.append("$.wave_id must be W00 through W18")
    if not is_string(obj.get("gate_id")) or not TASK_RE.fullmatch(obj.get("gate_id", "")):
        issues.append("$.gate_id must be a stable task/gate ID")
    if obj.get("status") not in STATUSES:
        issues.append("$.status has an invalid status")
    require_string(obj, "summary", "$", issues)
    if "artifact" in obj:
        validate_artifact(obj["artifact"], "$.artifact", issues)
    return issues


def validate_definition(value: Any) -> List[str]:
    issues: List[str] = []
    obj = require_object(value, "$", issues)
    if obj is None:
        return issues
    required = ("format_version", "migration_policy", "waves")
    require_keys(obj, required, "$", issues)
    check_format_version(obj, "$", issues)
    require_string(obj, "migration_policy", "$", issues)
    waves = require_array(obj.get("waves"), "$.waves", issues)
    if waves is None:
        return issues
    if len(waves) != 19:
        issues.append("$.waves must contain exactly W00 through W18")
    seen_waves = set()
    seen_tasks = set()
    expected_waves = {"W%02d" % number for number in range(19)}
    for index, item in enumerate(waves):
        path = "$.waves[%d]" % index
        wave = require_object(item, path, issues)
        if wave is None:
            continue
        fields = (
            "wave_id", "title", "goal", "dependencies", "parallel_tracks",
            "required_gate_ids", "optional_gate_ids", "responsible_paths", "roles",
            "expected_base_commit", "tasks",
        )
        require_keys(wave, fields, path, issues)
        require_string(wave, "title", path, issues)
        require_string(wave, "goal", path, issues)
        wave_id = wave.get("wave_id")
        if not is_string(wave_id) or not WAVE_RE.fullmatch(wave_id):
            issues.append("%s.wave_id must be W00 through W18" % path)
        elif wave_id in seen_waves:
            issues.append("%s.wave_id is duplicated" % path)
        else:
            seen_waves.add(wave_id)
        check_commit(wave.get("expected_base_commit"), "%s.expected_base_commit" % path, issues, nullable=True)
        for key in ("dependencies", "parallel_tracks", "required_gate_ids", "optional_gate_ids", "responsible_paths", "roles"):
            require_string_array(wave, key, path, issues)
        dependencies = wave.get("dependencies", [])
        if isinstance(dependencies, list):
            for dep_index, dependency in enumerate(dependencies):
                if not is_string(dependency) or not WAVE_RE.fullmatch(dependency):
                    issues.append("%s.dependencies[%d] must be a wave ID" % (path, dep_index))
        tasks = require_array(wave.get("tasks"), "%s.tasks" % path, issues)
        local_tasks = set()
        if tasks is not None:
            for task_index, task_value in enumerate(tasks):
                task_path = "%s.tasks[%d]" % (path, task_index)
                task = require_object(task_value, task_path, issues)
                if task is None:
                    continue
                require_keys(task, ("task_id", "title", "role", "owned_paths", "requires_clean_worktree"), task_path, issues)
                task_id = task.get("task_id")
                if not is_string(task_id) or not TASK_RE.fullmatch(task_id):
                    issues.append("%s.task_id is invalid" % task_path)
                elif is_string(wave_id) and not task_id.startswith(wave_id + "-"):
                    issues.append("%s.task_id must belong to %s" % (task_path, wave_id))
                elif task_id in seen_tasks:
                    issues.append("%s.task_id is duplicated" % task_path)
                else:
                    seen_tasks.add(task_id)
                    local_tasks.add(task_id)
                require_string(task, "title", task_path, issues)
                require_string(task, "role", task_path, issues)
                require_string_array(task, "owned_paths", task_path, issues)
                if "requires_clean_worktree" in task and not is_boolean(task["requires_clean_worktree"]):
                    issues.append("%s.requires_clean_worktree must be boolean" % task_path)
        required_ids = wave.get("required_gate_ids", [])
        optional_ids = wave.get("optional_gate_ids", [])
        if isinstance(required_ids, list) and isinstance(optional_ids, list):
            overlap = set(required_ids) & set(optional_ids)
            if overlap:
                issues.append("%s required and optional gate IDs overlap: %s" % (path, ", ".join(sorted(overlap))))
            undeclared = (set(required_ids) | set(optional_ids)) - local_tasks
            if undeclared:
                issues.append("%s gate IDs lack task definitions: %s" % (path, ", ".join(sorted(undeclared))))
    if seen_waves != expected_waves:
        issues.append("$.waves IDs must be exactly W00 through W18")
    return issues


def definition_indexes(definition: Dict[str, Any]) -> Tuple[Dict[str, Dict[str, Any]], Dict[str, Tuple[Dict[str, Any], Dict[str, Any]]]]:
    waves = {wave["wave_id"]: wave for wave in definition["waves"]}
    tasks: Dict[str, Tuple[Dict[str, Any], Dict[str, Any]]] = {}
    for wave in sorted(definition["waves"], key=lambda item: item["wave_id"]):
        for task in wave["tasks"]:
            tasks[task["task_id"]] = (wave, task)
    return waves, tasks


def validate_result(value: Any, definition: Dict[str, Any]) -> List[str]:
    issues: List[str] = []
    obj = require_object(value, "$", issues)
    if obj is None:
        return issues
    required = (
        "format_version", "task_id", "status", "expected_base_commit",
        "actual_base_commit", "head_commit", "branch", "changed_paths", "tests",
        "acceptance_criteria", "gate_evidence", "risks", "blockers",
        "worktree_clean", "timestamp",
    )
    require_keys(obj, required, "$", issues)
    allowed = set(required) | {"tested_head_commit", "seal_subject"}
    unknown = set(obj) - allowed
    if unknown:
        issues.append("$ has unknown fields: %s" % ", ".join(sorted(unknown)))
    check_format_version(obj, "$", issues)
    task_id = obj.get("task_id")
    _, task_index = definition_indexes(definition)
    if not is_string(task_id) or not TASK_RE.fullmatch(task_id):
        issues.append("$.task_id is invalid")
    elif task_id not in task_index:
        issues.append("$.task_id is unknown: %s" % task_id)
    if obj.get("status") not in STATUSES:
        issues.append("$.status has an invalid status")
    check_commit(obj.get("expected_base_commit"), "$.expected_base_commit", issues)
    check_commit(obj.get("actual_base_commit"), "$.actual_base_commit", issues)
    check_commit(obj.get("head_commit"), "$.head_commit", issues, nullable=True)
    if "tested_head_commit" in obj:
        check_commit(obj.get("tested_head_commit"), "$.tested_head_commit", issues)
    if "seal_subject" in obj:
        validate_seal_subject(obj.get("seal_subject"), "$.seal_subject", issues)
    if task_id == "W05-integration-gate":
        require_keys(obj, ("tested_head_commit", "seal_subject"), "$", issues)
        if obj.get("head_commit") is not None:
            issues.append(
                "$.head_commit must be null for the sealed W05 result"
            )
    require_string(obj, "branch", "$", issues)
    require_string_array(obj, "changed_paths", "$", issues)
    for index, item in enumerate(obj.get("changed_paths", []) if isinstance(obj.get("changed_paths"), list) else []):
        check_relative_path(item, "$.changed_paths[%d]" % index, issues)
    tests = require_array(obj.get("tests"), "$.tests", issues)
    if tests is not None:
        for index, item in enumerate(tests):
            path = "$.tests[%d]" % index
            test = require_object(item, path, issues)
            if test is None:
                continue
            require_keys(test, ("command", "status", "exit_code", "duration_ms", "artifact"), path, issues)
            require_string(test, "command", path, issues)
            if test.get("status") not in STATUSES:
                issues.append("%s.status has an invalid status" % path)
            if test.get("exit_code") is not None and not is_integer(test.get("exit_code")):
                issues.append("%s.exit_code must be an integer or null" % path)
            duration = test.get("duration_ms")
            if duration is not None and (not is_integer(duration) or duration < 0):
                issues.append("%s.duration_ms must be a non-negative integer or null" % path)
            if "artifact" in test:
                validate_artifact(test["artifact"], "%s.artifact" % path, issues)
    criteria = require_array(obj.get("acceptance_criteria"), "$.acceptance_criteria", issues)
    if criteria is not None:
        for index, item in enumerate(criteria):
            path = "$.acceptance_criteria[%d]" % index
            criterion = require_object(item, path, issues)
            if criterion is None:
                continue
            require_keys(criterion, ("criterion_id", "description", "status"), path, issues)
            require_string(criterion, "criterion_id", path, issues)
            require_string(criterion, "description", path, issues)
            if criterion.get("status") not in STATUSES:
                issues.append("%s.status has an invalid status" % path)
    evidence = require_array(obj.get("gate_evidence"), "$.gate_evidence", issues)
    if evidence is not None:
        for index, item in enumerate(evidence):
            nested_issues = validate_gate_evidence(item)
            issues.extend("$.gate_evidence[%d]%s" % (index, issue[1:]) for issue in nested_issues)
            if isinstance(item, dict) and item.get("gate_id") not in task_index:
                issues.append("$.gate_evidence[%d].gate_id is unknown: %s" % (index, item.get("gate_id")))
            elif isinstance(item, dict) and is_string(task_id) and task_id in task_index:
                expected_wave = task_index[task_id][0]["wave_id"]
                if item.get("wave_id") != expected_wave:
                    issues.append("$.gate_evidence[%d].wave_id must be %s" % (index, expected_wave))
    for key in ("risks", "blockers"):
        require_string_array(obj, key, "$", issues)
    if "worktree_clean" in obj and not is_boolean(obj["worktree_clean"]):
        issues.append("$.worktree_clean must be boolean")
    if "timestamp" in obj:
        check_timestamp(obj["timestamp"], "$.timestamp", issues)
    return issues


def validate_summary(value: Any, definition: Dict[str, Any]) -> List[str]:
    issues: List[str] = []
    obj = require_object(value, "$", issues)
    if obj is None:
        return issues
    required = ("format_version", "wave_id", "status", "required_gate_ids", "optional_gate_ids", "missing_gate_ids", "gate_statuses", "reasons")
    require_keys(obj, required, "$", issues)
    check_format_version(obj, "$", issues)
    waves, _ = definition_indexes(definition)
    if obj.get("wave_id") not in waves:
        issues.append("$.wave_id is unknown")
    if obj.get("status") not in SUMMARY_STATUSES:
        issues.append("$.status has an invalid summary status")
    for key in ("required_gate_ids", "optional_gate_ids", "missing_gate_ids", "reasons"):
        require_string_array(obj, key, "$", issues)
    gate_statuses = require_object(obj.get("gate_statuses"), "$.gate_statuses", issues)
    if gate_statuses is not None:
        for gate_id, status in gate_statuses.items():
            if not TASK_RE.fullmatch(gate_id):
                issues.append("$.gate_statuses has invalid gate ID %s" % gate_id)
            if status not in STATUSES:
                issues.append("$.gate_statuses.%s has invalid status" % gate_id)
    return issues


def load_definition(path: Path) -> Dict[str, Any]:
    value = read_json(path)
    issues = validate_definition(value)
    if issues:
        raise ValidationError("definition", issues)
    return value


def validate_ledger(value: Any, definition: Dict[str, Any]) -> List[str]:
    issues: List[str] = []
    obj = require_object(value, "$", issues)
    if obj is None:
        return issues
    require_keys(obj, ("format_version", "migration_policy", "waves"), "$", issues)
    check_format_version(obj, "$", issues)
    require_string(obj, "migration_policy", "$", issues)
    entries = require_array(obj.get("waves"), "$.waves", issues)
    if entries is None:
        return issues
    if len(entries) != 19:
        issues.append("$.waves must contain exactly W00 through W18")
    expected = {wave["wave_id"] for wave in definition["waves"]}
    seen = set()
    for index, item in enumerate(entries):
        path = "$.waves[%d]" % index
        entry = require_object(item, path, issues)
        if entry is None:
            continue
        require_keys(
            entry,
            (
                "wave_id", "state", "receipt_path", "receipt_sha256",
                "capabilities_provided", "semantic_debts", "codegen_eligible",
            ),
            path,
            issues,
        )
        wave_id = entry.get("wave_id")
        if wave_id not in expected:
            issues.append("%s.wave_id is unknown" % path)
        elif wave_id in seen:
            issues.append("%s.wave_id is duplicated" % path)
        else:
            seen.add(wave_id)
        state = entry.get("state")
        if state not in LEDGER_STATES:
            issues.append("%s.state is invalid" % path)
        receipt_path = entry.get("receipt_path")
        receipt_sha256 = entry.get("receipt_sha256")
        if state in {"revalidated", "sealed"}:
            check_relative_path(receipt_path, "%s.receipt_path" % path, issues)
            if not is_string(receipt_sha256) or not SHA256_RE.fullmatch(receipt_sha256):
                issues.append("%s.receipt_sha256 must be a lowercase SHA-256" % path)
        elif receipt_path is not None or receipt_sha256 is not None:
            issues.append("%s may bind a receipt only when revalidated or sealed" % path)
        for key in ("capabilities_provided", "semantic_debts"):
            require_string_array(entry, key, path, issues)
            values = entry.get(key)
            if isinstance(values, list) and len(values) != len(set(values)):
                issues.append("%s.%s must not contain duplicates" % (path, key))
        if entry.get("codegen_eligible") is not False:
            issues.append("%s.codegen_eligible must be false" % path)
    if seen != expected:
        issues.append("$.waves IDs must be exactly W00 through W18")
    return issues


def load_ledger(path: Path, definition: Dict[str, Any]) -> Dict[str, Any]:
    value = read_json(path)
    issues = validate_ledger(value, definition)
    if issues:
        raise ValidationError("ledger", issues)
    return value


def load_result(path: Path, definition: Dict[str, Any]) -> Dict[str, Any]:
    value = read_json(path)
    issues = validate_result(value, definition)
    if issues:
        raise ValidationError("result", issues)
    return value


def result_identity(result: Dict[str, Any]) -> Dict[str, Any]:
    return {
        key: result[key]
        for key in (
            "task_id",
            "expected_base_commit",
            "actual_base_commit",
            "head_commit",
            "tested_head_commit",
            "seal_subject",
            "branch",
        )
        if key in result
    }


def result_tested_head(result: Dict[str, Any]) -> Any:
    return result.get("tested_head_commit") or result.get("head_commit")


def result_wave(task_id: str, task_index: Dict[str, Tuple[Dict[str, Any], Dict[str, Any]]]) -> str:
    return task_index[task_id][0]["wave_id"]


def load_wave_results(results_dir: Path, wave_id: str, definition: Dict[str, Any]) -> Dict[str, Dict[str, Any]]:
    _, task_index = definition_indexes(definition)
    directory = results_dir / wave_id
    if not directory.exists():
        return {}
    results: Dict[str, Dict[str, Any]] = {}
    for path in sorted(directory.glob("*.json")):
        result = load_result(path, definition)
        task_id = result["task_id"]
        if result_wave(task_id, task_index) != wave_id:
            raise ValidationError("result", ["%s records %s outside %s" % (path, task_id, wave_id)])
        if task_id in results:
            raise ValidationError("result", ["duplicate result for %s" % task_id])
        results[task_id] = result
    return results


def task_failures(result: Dict[str, Any], wave: Dict[str, Any], task: Dict[str, Any]) -> List[str]:
    failures = []
    task_id = result["task_id"]
    if result["status"] != "pass":
        failures.append("%s status is %s" % (task_id, result["status"]))
    expected = wave.get("expected_base_commit")
    if expected is not None and result["expected_base_commit"] != expected:
        failures.append("%s declares stale expected base %s" % (task_id, result["expected_base_commit"]))
    if result["actual_base_commit"] != result["expected_base_commit"]:
        failures.append("%s actual base does not match expected base" % task_id)
    if result_tested_head(result) is None:
        failures.append("%s has no tested head commit" % task_id)
    if task["requires_clean_worktree"] and not result["worktree_clean"]:
        failures.append("%s implementation worktree is dirty" % task_id)
    if not result["tests"]:
        failures.append("%s has no recorded tests" % task_id)
    nonpassing_tests = [item["command"] for item in result["tests"] if item["status"] != "pass" or item["exit_code"] != 0]
    if nonpassing_tests:
        failures.append("%s has non-passing tests: %s" % (task_id, ", ".join(nonpassing_tests)))
    if not result["acceptance_criteria"]:
        failures.append("%s has no acceptance criteria" % task_id)
    nonpassing_criteria = [item["criterion_id"] for item in result["acceptance_criteria"] if item["status"] != "pass"]
    if nonpassing_criteria:
        failures.append("%s has non-passing acceptance criteria: %s" % (task_id, ", ".join(nonpassing_criteria)))
    matching_evidence = [
        item for item in result["gate_evidence"]
        if item["gate_id"] == task_id and item["wave_id"] == wave["wave_id"]
    ]
    if not matching_evidence or not any(item["status"] == "pass" for item in matching_evidence):
        failures.append("%s lacks passing gate evidence" % task_id)
    if any(item["status"] != "pass" for item in matching_evidence):
        failures.append("%s has contradictory non-passing gate evidence" % task_id)
    if result["blockers"]:
        failures.append("%s reports blockers: %s" % (task_id, "; ".join(result["blockers"])))
    return failures


def aggregate_wave(wave: Dict[str, Any], results: Dict[str, Dict[str, Any]]) -> Dict[str, Any]:
    task_defs = {task["task_id"]: task for task in wave["tasks"]}
    required = list(wave["required_gate_ids"])
    optional = list(wave["optional_gate_ids"])
    missing = [task_id for task_id in required if task_id not in results]
    reasons: List[str] = []
    gate_statuses = {task_id: result["status"] for task_id, result in sorted(results.items())}
    for task_id in required:
        result = results.get(task_id)
        if result is None:
            continue
        failures = task_failures(result, wave, task_defs[task_id])
        reasons.extend(failures)
    if missing:
        status = "missing"
        reasons.insert(0, "missing required gates: %s" % ", ".join(missing))
    elif reasons:
        statuses = {results[task_id]["status"] for task_id in required}
        if "fail" in statuses or "skipped" in statuses:
            status = "fail"
        elif "blocked" in statuses:
            status = "blocked"
        elif "running" in statuses:
            status = "running"
        elif "pending" in statuses:
            status = "pending"
        else:
            status = "fail"
    else:
        status = "pass"
    return {
        "format_version": FORMAT_VERSION,
        "wave_id": wave["wave_id"],
        "status": status,
        "required_gate_ids": required,
        "optional_gate_ids": optional,
        "missing_gate_ids": missing,
        "gate_statuses": gate_statuses,
        "reasons": reasons,
    }


def json_stdout(value: Any) -> None:
    json.dump(value, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")


def command_validate_definition(args: argparse.Namespace) -> int:
    definition = load_definition(args.definition)
    print("valid definition: %d waves" % len(definition["waves"]))
    return EXIT_OK


def command_validate_result(args: argparse.Namespace) -> int:
    definition = load_definition(args.definition)
    result = load_result(args.file, definition)
    print("valid result: %s" % result["task_id"])
    return EXIT_OK


def command_validate_evidence(args: argparse.Namespace) -> int:
    definition = load_definition(args.definition)
    _, task_index = definition_indexes(definition)
    value = read_json(args.file)
    issues = validate_gate_evidence(value)
    if isinstance(value, dict) and value.get("gate_id") not in task_index:
        issues.append("$.gate_id is unknown: %s" % value.get("gate_id"))
    if issues:
        raise ValidationError("gate evidence", issues)
    print("valid gate evidence: %s" % value["gate_id"])
    return EXIT_OK


def command_record(args: argparse.Namespace) -> int:
    definition = load_definition(args.definition)
    _, task_index = definition_indexes(definition)
    result = load_result(args.result, definition)
    if result_wave(result["task_id"], task_index) != args.wave:
        raise ValidationError("result", ["%s does not belong to %s" % (result["task_id"], args.wave)])
    destination = args.results_dir / args.wave / (result["task_id"] + ".json")
    replaced = False
    if destination.exists():
        existing = load_result(destination, definition)
        if existing == result:
            json_stdout({"path": str(destination), "recorded": False, "reason": "identical"})
            return EXIT_OK
        if not args.replace:
            raise ValidationError(
                "duplicate",
                ["result exists with different content/identity; use --replace to create an audited replacement"],
            )
        audit_path = args.results_dir / ".audit.json"
        audit = read_json(audit_path) if audit_path.exists() else {"format_version": FORMAT_VERSION, "replacements": []}
        if not isinstance(audit, dict) or not isinstance(audit.get("replacements"), list):
            raise ValidationError("audit", ["existing audit file is structurally invalid"])
        audit["replacements"].append({
            "wave_id": args.wave,
            "task_id": result["task_id"],
            "old_identity": result_identity(existing),
            "new_identity": result_identity(result),
            "replaced_at": dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        })
        write_json_atomic(audit_path, audit)
        replaced = True
    write_json_atomic(destination, result)
    json_stdout({"path": str(destination), "recorded": True, "replaced": replaced})
    return EXIT_OK


def command_check_results(args: argparse.Namespace) -> int:
    definition = load_definition(args.definition)
    waves, _ = definition_indexes(definition)
    results = load_wave_results(args.results_dir, args.wave, definition)
    summary = aggregate_wave(waves[args.wave], results)
    summary_issues = validate_summary(summary, definition)
    if summary_issues:
        raise ValidationError("summary", summary_issues)
    json_stdout(summary)
    return EXIT_OK if summary["status"] == "pass" else EXIT_GATE_FAIL


def command_check(args: argparse.Namespace) -> int:
    if args.results_dir is not None:
        return command_check_results(args)
    definition = load_definition(args.definition)
    ledger = load_ledger(args.ledger, definition)
    entry = next(item for item in ledger["waves"] if item["wave_id"] == args.wave)
    json_stdout(entry)
    return EXIT_OK if entry["state"] in {"revalidated", "sealed"} else EXIT_GATE_FAIL


def command_list_missing(args: argparse.Namespace) -> int:
    definition = load_definition(args.definition)
    waves, _ = definition_indexes(definition)
    results = load_wave_results(args.results_dir, args.wave, definition)
    missing = [task_id for task_id in waves[args.wave]["required_gate_ids"] if task_id not in results]
    for task_id in missing:
        print(task_id)
    return EXIT_GATE_FAIL if missing else EXIT_OK


def markdown_cell(value: Any) -> str:
    if value is None or value == "":
        return "—"
    return str(value).replace("|", "\\|").replace("\r", " ").replace("\n", " ")


def short_commit(value: Any) -> str:
    return value[:12] if is_string(value) else "—"


def render_dashboard(definition: Dict[str, Any], results_dir: Path, include_timestamps: bool) -> str:
    lines = [
        "<!-- Generated by scripts/native/wave-gate.py; do not edit. -->",
        "# Native engine wave status",
        "",
        "This dashboard is a deterministic projection of `waves.json` and recorded task results.",
        "",
    ]
    for wave in sorted(definition["waves"], key=lambda item: item["wave_id"]):
        results = load_wave_results(results_dir, wave["wave_id"], definition)
        summary = aggregate_wave(wave, results)
        lines.extend([
            "## %s — %s" % (wave["wave_id"], wave["title"]),
            "",
            "**Status:** `%s`" % summary["status"].upper(),
            "",
            wave["goal"],
            "",
        ])
        if summary["missing_gate_ids"]:
            lines.extend(["**Missing required gates:** %s" % ", ".join("`%s`" % item for item in summary["missing_gate_ids"]), ""])
        visible_reasons = [reason for reason in summary["reasons"] if not reason.startswith("missing required gates:")]
        if visible_reasons:
            lines.extend(["**Gate reasons:** %s" % markdown_cell("; ".join(visible_reasons)), ""])
        headers = ["Task", "Required", "Status", "Base", "Head", "Evidence", "Blockers"]
        if include_timestamps:
            headers.append("Timestamp")
        lines.append("| " + " | ".join(headers) + " |")
        lines.append("| " + " | ".join(["---"] * len(headers)) + " |")
        required_set = set(wave["required_gate_ids"])
        for task in sorted(wave["tasks"], key=lambda item: item["task_id"]):
            task_id = task["task_id"]
            result = results.get(task_id)
            if result is None:
                row = [task_id, "yes" if task_id in required_set else "no", "missing", "—", "—", "—", "—"]
                if include_timestamps:
                    row.append("—")
            else:
                evidence = result["gate_evidence"]
                evidence_text = "; ".join(
                    "%s (%s)" % (item["summary"], item["artifact"]["reference"])
                    for item in evidence
                ) or "—"
                row = [
                    task_id,
                    "yes" if task_id in required_set else "no",
                    result["status"],
                    short_commit(result["actual_base_commit"]),
                    short_commit(result_tested_head(result)),
                    evidence_text,
                    "; ".join(result["blockers"]) or "—",
                ]
                if include_timestamps:
                    row.append(result["timestamp"])
            lines.append("| " + " | ".join(markdown_cell(item) for item in row) + " |")
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def render_ledger_dashboard(definition: Dict[str, Any], ledger: Dict[str, Any]) -> str:
    definition_waves, _ = definition_indexes(definition)
    lines = [
        "<!-- Generated by scripts/native/wave-gate.py; do not edit. -->",
        "# Native engine wave status",
        "",
        "This dashboard is a deterministic projection of committed `ledger.json` receipts.",
        "",
    ]
    for entry in sorted(ledger["waves"], key=lambda item: item["wave_id"]):
        wave = definition_waves[entry["wave_id"]]
        lines.extend([
            "## %s — %s" % (entry["wave_id"], wave["title"]),
            "",
            "**Status:** `%s`" % entry["state"].upper(),
            "",
            wave["goal"],
            "",
        ])
        if entry["receipt_path"] is not None:
            lines.extend([
                "**Receipt:** `%s` (`%s`)" % (
                    entry["receipt_path"], entry["receipt_sha256"],
                ),
                "",
            ])
        capabilities = entry["capabilities_provided"]
        debts = entry["semantic_debts"]
        lines.extend([
            "**Capabilities:** %s" % (
                ", ".join("`%s`" % item for item in capabilities) if capabilities else "none recorded"
            ),
            "",
            "**Semantic debts:** %s" % (
                ", ".join("`%s`" % item for item in debts) if debts else "none recorded"
            ),
            "",
            "**Codegen eligible:** `false`",
            "",
        ])
    return "\n".join(lines).rstrip() + "\n"


def command_render_results(args: argparse.Namespace) -> int:
    definition = load_definition(args.definition)
    dashboard = render_dashboard(definition, args.results_dir, args.include_timestamps)
    write_text_atomic(args.output, dashboard)
    print("rendered %s" % args.output)
    return EXIT_OK


def command_render(args: argparse.Namespace) -> int:
    if args.results_dir is not None:
        return command_render_results(args)
    definition = load_definition(args.definition)
    ledger = load_ledger(args.ledger, definition)
    dashboard = render_ledger_dashboard(definition, ledger)
    write_text_atomic(args.output, dashboard)
    print("rendered %s" % args.output)
    return EXIT_OK


def add_common_definition_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--definition", type=Path, default=DEFAULT_DEFINITION, help="wave definition JSON (default: repository waves.json)")


def build_parser() -> UsageParser:
    parser = UsageParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    validate_definition_parser = subparsers.add_parser("validate-definition", help="validate waves.json")
    add_common_definition_argument(validate_definition_parser)
    validate_definition_parser.set_defaults(handler=command_validate_definition)

    validate_result_parser = subparsers.add_parser("validate-result", help="validate a task result")
    validate_result_parser.add_argument("file", type=Path)
    add_common_definition_argument(validate_result_parser)
    validate_result_parser.set_defaults(handler=command_validate_result)

    validate_evidence_parser = subparsers.add_parser("validate-evidence", help="validate standalone gate evidence")
    validate_evidence_parser.add_argument("file", type=Path)
    add_common_definition_argument(validate_evidence_parser)
    validate_evidence_parser.set_defaults(handler=command_validate_evidence)

    record_parser = subparsers.add_parser("record", help="atomically record a task result")
    record_parser.add_argument("--wave", required=True, choices=["W%02d" % number for number in range(19)])
    record_parser.add_argument("--result", required=True, type=Path)
    record_parser.add_argument("--results-dir", required=True, type=Path)
    record_parser.add_argument("--replace", action="store_true", help="replace differing result and append audit record")
    add_common_definition_argument(record_parser)
    record_parser.set_defaults(handler=command_record)

    check_parser = subparsers.add_parser("check", help="evaluate a committed ledger entry")
    check_parser.add_argument("--wave", required=True, choices=["W%02d" % number for number in range(19)])
    check_parser.add_argument("--ledger", type=Path, default=DEFAULT_LEDGER)
    check_parser.add_argument("--results-dir", type=Path, help=argparse.SUPPRESS)
    add_common_definition_argument(check_parser)
    check_parser.set_defaults(handler=command_check)

    check_results_parser = subparsers.add_parser("check-results", help="evaluate external task results")
    check_results_parser.add_argument("--wave", required=True, choices=["W%02d" % number for number in range(19)])
    check_results_parser.add_argument("--results-dir", required=True, type=Path)
    add_common_definition_argument(check_results_parser)
    check_results_parser.set_defaults(handler=command_check_results)

    render_parser = subparsers.add_parser("render", help="render committed ledger dashboard")
    render_parser.add_argument("--ledger", type=Path, default=DEFAULT_LEDGER)
    render_parser.add_argument("--results-dir", type=Path, help=argparse.SUPPRESS)
    render_parser.add_argument("--output", required=True, type=Path)
    render_parser.add_argument("--include-timestamps", action="store_true")
    add_common_definition_argument(render_parser)
    render_parser.set_defaults(handler=command_render)

    render_results_parser = subparsers.add_parser("render-results", help="render external task-result dashboard")
    render_results_parser.add_argument("--results-dir", required=True, type=Path)
    render_results_parser.add_argument("--output", required=True, type=Path)
    render_results_parser.add_argument("--include-timestamps", action="store_true")
    add_common_definition_argument(render_results_parser)
    render_results_parser.set_defaults(handler=command_render_results)

    missing_parser = subparsers.add_parser("list-missing", help="list missing required gate IDs")
    missing_parser.add_argument("--wave", required=True, choices=["W%02d" % number for number in range(19)])
    missing_parser.add_argument("--results-dir", required=True, type=Path)
    add_common_definition_argument(missing_parser)
    missing_parser.set_defaults(handler=command_list_missing)
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.handler(args)
    except ValidationError as exc:
        print("%s invalid:" % exc.kind, file=sys.stderr)
        for issue in exc.issues:
            print("- %s" % issue, file=sys.stderr)
        return EXIT_SCHEMA_INVALID
    except OSError as exc:
        print("I/O error: %s" % exc, file=sys.stderr)
        return EXIT_SCHEMA_INVALID


if __name__ == "__main__":
    sys.exit(main())
