#!/usr/bin/env python3
"""Cross-validate the complete W01 semantic contract without dependencies."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable


ROOT = Path(__file__).resolve().parents[3]
REPORT_PATH = Path("docs/native-engine/semantics/w01-coverage-report.json")
WAVE_BASE = "dc6e34b56846c38dc2475d6c962c2b9b7ada6df4"
TPDE_PIN = "338d41890e424b058e2053b6a5787e1348e3dd57"

ARTIFACTS = {
    "blockers": Path("docs/native-engine/semantics/blockers.json"),
    "cross_contract": Path("docs/native-engine/waves/w01-cross-track-contract.md"),
    "effects": Path("docs/native-engine/semantics/effects/effect-model.json"),
    "frame_examples": Path("docs/native-engine/semantics/frames/frame-state.examples.json"),
    "frame_schema": Path("docs/native-engine/semantics/frames/frame-state.schema.json"),
    "manifest": Path("tests/native/semantics/manifest.json"),
    "opcodes": Path("docs/native-engine/semantics/opcodes/opcode-matrix.json"),
    "tpde": Path("docs/native-engine/tpde/required-capabilities.json"),
}

FROZEN_EFFECTS = {
    "read_memory", "write_memory", "allocate", "throw", "bailout",
    "call_internal", "call_php", "reenter_php", "run_destructor",
    "observe_frame", "interrupt_boundary", "suspend", "resume",
    "external_io", "terminate",
}
FROZEN_DOMAINS = {
    "frame.args", "frame.locals", "frame.temps", "frame.call_chain",
    "runtime.symbol_table", "runtime.cache", "heap.zval", "heap.array",
    "heap.object", "heap.string", "heap.reference", "gc.metadata",
    "engine.exception", "engine.observer", "engine.interrupt",
    "engine.class_table", "engine.function_table", "engine.generator",
    "engine.fiber", "external.state",
}
FROZEN_ACTIONS = {
    "borrow", "copy_addref", "move", "produce_owned", "produce_borrowed",
    "transfer", "destroy", "conditional_destroy", "cow_separate",
    "canonicalize",
}
FROZEN_BARRIERS = {
    "safepoint", "reentrancy", "destructor", "exception", "bailout",
    "observer", "interrupt", "suspend",
}
REQUIRED_FAMILIES = {
    "arrays_cow", "assignments_temps", "calls_arguments_returns",
    "closures_dynamic_calls", "compare_branch_switch",
    "eval_include_require", "exceptions_finally", "fibers", "generators",
    "interrupt_ticks", "objects_properties_magic",
    "observer_user_opcode_hooks", "references_indirects",
    "scalars_type_juggling", "strings", "zts_relevant_paths",
}
REQUIRED_RISKS = {
    "bailout_nonlocal_transfer", "destructor_reentrancy",
    "exception_finally_order", "fiber_suspend_resume",
    "generator_suspend_resume_close", "indirect_reference_aliasing",
    "observer_begin_end",
}
REQUIRED_SAFEPOINTS = {
    "function_entry", "user_call", "internal_call", "allocation",
    "destructor", "exception_edge", "bailout_helper", "observer",
    "interrupt", "generator_suspend", "generator_resume", "fiber_switch",
    "deopt_resume",
}
REQUIRED_TPDE_CAPABILITIES = {
    "custom-mapper", "force-materialize", "frame-stackmap-metadata",
    "resume-osr", "statepoint-code-offset", "value-location-snapshot",
}
PLACEHOLDER = re.compile(r"\b(?:unknown|TBD|later|probably)\b", re.IGNORECASE)
IDENTIFIER = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")


class ValidationError(ValueError):
    """A deterministic W01 cross-contract validation failure."""


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValidationError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_json(path: Path) -> Any:
    try:
        with path.open(encoding="utf-8") as handle:
            return json.load(handle, object_pairs_hook=_reject_duplicate_keys)
    except (OSError, json.JSONDecodeError) as exc:
        raise ValidationError(f"cannot load {path}: {exc}") from exc


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValidationError(message)


def require_id_set(items: Any, label: str) -> set[str]:
    require(isinstance(items, list), f"{label} must be an array")
    identifiers: list[str] = []
    for index, item in enumerate(items):
        require(isinstance(item, dict), f"{label}[{index}] must be an object")
        identifier = item.get("id")
        require(
            isinstance(identifier, str) and IDENTIFIER.fullmatch(identifier) is not None,
            f"{label}[{index}].id is invalid",
        )
        identifiers.append(identifier)
    require(len(identifiers) == len(set(identifiers)), f"{label} contains duplicate IDs")
    return set(identifiers)


def require_subset(values: Any, registered: set[str], label: str) -> None:
    require(isinstance(values, list), f"{label} must be an array")
    require(all(isinstance(value, str) for value in values), f"{label} must contain strings")
    missing = sorted(set(values) - registered)
    require(not missing, f"{label} references unregistered IDs: {missing}")


def walk_strings(value: Any, path: str = "$") -> Iterable[tuple[str, str]]:
    if isinstance(value, str):
        yield path, value
    elif isinstance(value, list):
        for index, item in enumerate(value):
            yield from walk_strings(item, f"{path}[{index}]")
    elif isinstance(value, dict):
        for key, item in value.items():
            yield from walk_strings(item, f"{path}.{key}")


def find_safepoint_enum(value: Any) -> set[str]:
    if isinstance(value, dict):
        enum = value.get("enum")
        if isinstance(enum, list) and "function_entry" in enum:
            return {item for item in enum if isinstance(item, str)}
        for item in value.values():
            found = find_safepoint_enum(item)
            if found:
                return found
    elif isinstance(value, list):
        for item in value:
            found = find_safepoint_enum(item)
            if found:
                return found
    return set()


def validate_blockers(document: Any) -> tuple[list[dict[str, Any]], list[str]]:
    require(isinstance(document, dict), "blockers document must be an object")
    require(set(document) == {"$schema", "blockers", "format_version", "wave_id"}, "blockers document has unexpected fields")
    require(document["$schema"] == "./blockers.schema.json", "blockers schema reference is invalid")
    require(document["format_version"] == "1.0.0", "blockers format_version must be 1.0.0")
    require(document["wave_id"] == "W01", "blockers wave_id must be W01")
    blockers = document["blockers"]
    require(isinstance(blockers, list), "blockers must be an array")
    identifiers: list[str] = []
    unresolved_critical: list[str] = []
    required_fields = {
        "affected_waves", "blocker_id", "decision", "github_issue_url",
        "issue_ready", "owner", "severity", "source_refs", "status", "title",
    }
    for index, blocker in enumerate(blockers):
        context = f"blockers[{index}]"
        require(isinstance(blocker, dict), f"{context} must be an object")
        require(set(blocker) == required_fields, f"{context} has unexpected fields")
        blocker_id = blocker["blocker_id"]
        require(isinstance(blocker_id, str) and re.fullmatch(r"W01-BLOCK-[0-9]{3}", blocker_id) is not None, f"{context}.blocker_id is invalid")
        identifiers.append(blocker_id)
        require(blocker["severity"] in {"critical", "high", "medium", "low"}, f"{context}.severity is invalid")
        require(blocker["status"] in {"resolved", "unresolved"}, f"{context}.status is invalid")
        require(isinstance(blocker["owner"], str) and blocker["owner"], f"{context}.owner is required")
        require(isinstance(blocker["title"], str) and blocker["title"], f"{context}.title is required")
        require(isinstance(blocker["decision"], str) and blocker["decision"], f"{context}.decision is required")
        require(isinstance(blocker["source_refs"], list) and blocker["source_refs"], f"{context}.source_refs is required")
        require(isinstance(blocker["affected_waves"], list) and blocker["affected_waves"], f"{context}.affected_waves is required")
        require(all(isinstance(wave, str) and re.fullmatch(r"W[0-9]{2}", wave) for wave in blocker["affected_waves"]), f"{context}.affected_waves is invalid")
        issue_url = blocker["github_issue_url"]
        require(issue_url is None or (isinstance(issue_url, str) and issue_url.startswith("https://github.com/")), f"{context}.github_issue_url is invalid")
        require(isinstance(blocker["issue_ready"], bool), f"{context}.issue_ready must be boolean")
        if blocker["status"] == "unresolved" and blocker["severity"] == "critical":
            require(issue_url is not None, f"{context} unresolved critical blocker lacks a GitHub issue")
            unresolved_critical.append(blocker_id)
    require(len(identifiers) == len(set(identifiers)), "blockers contain duplicate IDs")
    return blockers, unresolved_critical


def build_report(root: Path = ROOT) -> dict[str, Any]:
    documents = {
        name: load_json(root / relative)
        for name, relative in ARTIFACTS.items()
        if relative.suffix == ".json"
    }
    try:
        cross_contract = (root / ARTIFACTS["cross_contract"]).read_text(encoding="utf-8")
    except OSError as exc:
        raise ValidationError(f"cannot load {root / ARTIFACTS['cross_contract']}: {exc}") from exc

    matrix = documents["opcodes"]
    require(matrix.get("format_version") == "1.0.0", "opcode matrix format_version must be 1.0.0")
    require(matrix.get("source_commit") == WAVE_BASE, "opcode matrix source commit is stale")
    opcodes = matrix.get("opcodes")
    require(isinstance(opcodes, list) and opcodes, "opcode matrix must contain opcodes")
    opcode_names = [opcode.get("opcode") for opcode in opcodes if isinstance(opcode, dict)]
    opcode_numbers = [opcode.get("number") for opcode in opcodes if isinstance(opcode, dict)]
    require(len(opcode_names) == len(opcodes), "opcode matrix contains a non-object entry")
    require(len(opcode_names) == len(set(opcode_names)), "opcode matrix contains duplicate opcode names")
    require(len(opcode_numbers) == len(set(opcode_numbers)), "opcode matrix contains duplicate opcode numbers")
    require(all(isinstance(name, str) and name.startswith("ZEND_") for name in opcode_names), "opcode matrix contains an invalid opcode ID")
    sentinel = matrix.get("sentinel")
    require(isinstance(sentinel, dict) and isinstance(sentinel.get("value"), int), "opcode matrix sentinel is invalid")
    reserved = matrix.get("reserved_opcode_numbers")
    require(isinstance(reserved, list) and all(isinstance(number, int) for number in reserved), "reserved opcode numbers are invalid")
    expected_numbers = set(range(sentinel["value"] + 1)) - set(reserved)
    require(set(opcode_numbers) == expected_numbers, "opcode matrix does not cover every executable opcode number exactly once")

    model = documents["effects"]
    effects = require_id_set(model.get("atomic_effects"), "atomic_effects")
    domains = require_id_set(model.get("memory_domains"), "memory_domains")
    actions = require_id_set(model.get("ownership_actions"), "ownership_actions")
    barriers = require_id_set(model.get("barriers"), "barriers")
    for actual, required, label in (
        (effects, FROZEN_EFFECTS, "effects"),
        (domains, FROZEN_DOMAINS, "memory domains"),
        (actions, FROZEN_ACTIONS, "ownership actions"),
        (barriers, FROZEN_BARRIERS, "barriers"),
    ):
        missing = sorted(required - actual)
        require(not missing, f"{label} omit frozen IDs: {missing}")
        absent_from_contract = sorted(identifier for identifier in required if f"`{identifier}`" not in cross_contract)
        require(not absent_from_contract, f"cross-track contract omits {label}: {absent_from_contract}")

    missing_crosslinks: list[str] = []
    for opcode in opcodes:
        name = opcode["opcode"]
        require(isinstance(opcode.get("source_refs"), list) and opcode["source_refs"], f"{name} lacks source refs")
        require_subset(opcode.get("effect_ids"), effects, f"{name}.effect_ids")
        require_subset(opcode.get("read_domains"), domains, f"{name}.read_domains")
        require_subset(opcode.get("write_domains"), domains, f"{name}.write_domains")
        require_subset(opcode.get("ownership_actions"), actions, f"{name}.ownership_actions")
        require_subset(opcode.get("barrier_ids"), barriers, f"{name}.barrier_ids")
        if "run_destructor" in opcode["effect_ids"] and "destructor" not in opcode["barrier_ids"]:
            missing_crosslinks.append(f"{name}:destructor-barrier")
        if "bailout" in opcode["effect_ids"] and "bailout" not in opcode["barrier_ids"]:
            missing_crosslinks.append(f"{name}:bailout-barrier")
        if ({"call_php", "reenter_php"} & set(opcode["effect_ids"])) and "reentrancy" not in opcode["barrier_ids"]:
            missing_crosslinks.append(f"{name}:reentrancy-barrier")
    require(not missing_crosslinks, f"opcode/effect crosslinks are incomplete: {missing_crosslinks}")

    manifest = documents["manifest"]
    require(manifest.get("wave_base_commit") == WAVE_BASE, "semantic manifest wave base is stale")
    declared_families = set(manifest.get("required_opcode_families", []))
    require(declared_families == REQUIRED_FAMILIES, f"semantic manifest required families differ: {sorted(declared_families ^ REQUIRED_FAMILIES)}")
    declared_risks = set(manifest.get("required_semantic_risks", []))
    require(REQUIRED_RISKS <= declared_risks, f"semantic manifest omits critical risks: {sorted(REQUIRED_RISKS - declared_risks)}")
    fixtures = manifest.get("fixtures")
    require(isinstance(fixtures, list) and fixtures, "semantic manifest must contain fixtures")
    family_fixtures = {family: [] for family in sorted(REQUIRED_FAMILIES)}
    risk_fixtures = {risk: [] for risk in sorted(declared_risks)}
    for fixture in fixtures:
        require(isinstance(fixture, dict), "semantic manifest contains a non-object fixture")
        fixture_id = fixture.get("fixture_id")
        require(isinstance(fixture_id, str) and fixture_id, "semantic fixture lacks fixture_id")
        for family in fixture.get("opcode_families", []):
            require(family in family_fixtures, f"{fixture_id} references undeclared family {family}")
            family_fixtures[family].append(fixture_id)
        for risk in fixture.get("semantic_risks", []):
            require(risk in risk_fixtures, f"{fixture_id} references undeclared risk {risk}")
            risk_fixtures[risk].append(fixture_id)
        unknown_opcodes = sorted(set(fixture.get("opcodes", [])) - set(opcode_names))
        require(not unknown_opcodes, f"{fixture_id} references unknown opcodes: {unknown_opcodes}")
    uncovered_families = sorted(family for family, covered_by in family_fixtures.items() if not covered_by)
    uncovered_risks = sorted(risk for risk in REQUIRED_RISKS if not risk_fixtures[risk])
    require(not uncovered_families, f"opcode families lack fixtures: {uncovered_families}")
    require(not uncovered_risks, f"critical semantic risks lack fixtures: {uncovered_risks}")

    frame_schema = documents["frame_schema"]
    safepoints = find_safepoint_enum(frame_schema)
    require(REQUIRED_SAFEPOINTS <= safepoints, f"frame schema omits safepoints: {sorted(REQUIRED_SAFEPOINTS - safepoints)}")
    frame_examples = documents["frame_examples"].get("examples")
    require(isinstance(frame_examples, list) and frame_examples, "frame examples are missing")
    valid_frame_examples = [example["example_id"] for example in frame_examples if example.get("expected_valid") is True]
    require(valid_frame_examples, "frame contract has no valid examples")

    tpde = documents["tpde"]
    require(tpde.get("tpde_commit") == TPDE_PIN, "TPDE capability analysis uses the wrong pin")
    capability_ids = require_id_set(tpde.get("capabilities"), "TPDE capabilities")
    require(REQUIRED_TPDE_CAPABILITIES <= capability_ids, f"TPDE analysis omits integration capabilities: {sorted(REQUIRED_TPDE_CAPABILITIES - capability_ids)}")

    blockers, unresolved_critical = validate_blockers(documents["blockers"])
    blocker_ids = {blocker["blocker_id"] for blocker in blockers}
    blocked_capabilities: list[str] = []
    for capability in tpde["capabilities"]:
        require(isinstance(capability.get("source_refs"), list) and capability["source_refs"], f"TPDE capability {capability['id']} lacks source refs")
        blocker_id = capability.get("blocker_id")
        if blocker_id is not None:
            require(blocker_id in blocker_ids, f"TPDE capability {capability['id']} references unknown blocker {blocker_id}")
        if capability.get("critical") is True and capability.get("classification") == "blocked":
            blocked_capabilities.append(capability["id"])
    require(not blocked_capabilities, f"critical TPDE capabilities remain blocked: {sorted(blocked_capabilities)}")
    require(not unresolved_critical, f"critical W01 blockers remain unresolved: {sorted(unresolved_critical)}")

    for artifact_name, document in sorted(documents.items()):
        for path, value in walk_strings(document):
            require(PLACEHOLDER.search(value) is None, f"{artifact_name}{path} contains a semantic placeholder")

    return {
        "blockers": {
            "resolved": sum(blocker["status"] == "resolved" for blocker in blockers),
            "total": len(blockers),
            "unresolved_critical": [],
        },
        "cross_links": {
            "barrier_ids": len(barriers),
            "effect_ids": len(effects),
            "memory_domain_ids": len(domains),
            "missing": [],
            "ownership_action_ids": len(actions),
        },
        "format_version": "1.0.0",
        "frame_contract": {
            "required_safepoints": sorted(REQUIRED_SAFEPOINTS),
            "valid_examples": sorted(valid_frame_examples),
        },
        "opcode_coverage": {
            "catalogued": len(opcodes),
            "first_number": min(opcode_numbers),
            "last_number": max(opcode_numbers),
            "unique_ids": len(set(opcode_names)),
        },
        "semantic_risk_coverage": {
            risk: sorted(risk_fixtures[risk]) for risk in sorted(REQUIRED_RISKS)
        },
        "status": "pass",
        "test_family_coverage": {
            family: sorted(family_fixtures[family]) for family in sorted(REQUIRED_FAMILIES)
        },
        "tpde_contract": {
            "capabilities": len(capability_ids),
            "required_integration_capabilities": sorted(REQUIRED_TPDE_CAPABILITIES),
            "tpde_commit": TPDE_PIN,
        },
        "wave_base_commit": WAVE_BASE,
        "wave_id": "W01",
    }


def render_report(report: dict[str, Any]) -> str:
    return json.dumps(report, indent=2, sort_keys=True) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="validate and require the checked-in report to be current")
    parser.add_argument("--output", type=Path, help="write the deterministic report to this path")
    parser.add_argument("--root", type=Path, default=ROOT, help="repository-like artifact root used by contract tests")
    args = parser.parse_args(argv)
    try:
        report = build_report(args.root.resolve())
        rendered = render_report(report)
        if args.check:
            checked_in = args.root.resolve() / REPORT_PATH
            try:
                actual = checked_in.read_text(encoding="utf-8")
            except OSError as exc:
                raise ValidationError(f"cannot load {checked_in}: {exc}") from exc
            require(actual == rendered, f"{REPORT_PATH} is stale; regenerate it")
        if args.output is not None:
            args.output.write_text(rendered, encoding="utf-8")
        elif not args.check:
            (args.root.resolve() / REPORT_PATH).write_text(rendered, encoding="utf-8")
    except ValidationError as exc:
        print(f"W01 validation failed: {exc}", file=sys.stderr)
        return 1
    print(f"W01 semantic contracts pass ({report['opcode_coverage']['catalogued']} opcodes, {len(report['test_family_coverage'])} families)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
