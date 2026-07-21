#!/usr/bin/env python3
"""Cross-validate the complete W01 semantic contract without dependencies."""

from __future__ import annotations

import argparse
import contextlib
import importlib.util
import io
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable


ROOT = Path(__file__).resolve().parents[3]
OPCODE_SOURCE_COMMIT = "b7c524a19fa815799a858b98d39f176ca88648b1"
TPDE_PIN = "338d41890e424b058e2053b6a5787e1348e3dd57"

ARTIFACTS = {
    "vocabulary": Path("docs/native-engine/semantics/vocabulary.md"),
    "effects": Path("docs/native-engine/semantics/effects/effect-model.json"),
    "effects_schema": Path("docs/native-engine/semantics/effects/effect-model.schema.json"),
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


def load_validator_module(name: str, relative: str) -> Any:
    path = ROOT / relative
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise ValidationError(f"cannot load validator {relative}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


OPCODE_VALIDATOR = load_validator_module(
    "w01_opcode_validator", "scripts/native/semantics/generate-opcode-matrix.py"
)
EFFECT_VALIDATOR = load_validator_module(
    "w01_effect_validator", "scripts/native/semantics/validate-effect-model.py"
)
FRAME_VALIDATOR = load_validator_module(
    "w01_frame_validator", "scripts/native/semantics/validate-frame-contract.py"
)
TPDE_VALIDATOR = load_validator_module(
    "w01_tpde_validator", "scripts/native/semantics/validate-tpde-gap.py"
)
MANIFEST_VALIDATOR = load_validator_module(
    "w01_manifest_validator", "tests/native/semantics/validate_manifest.py"
)


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


def build_report(root: Path = ROOT) -> dict[str, Any]:
    documents = {
        name: load_json(root / relative)
        for name, relative in ARTIFACTS.items()
        if relative.suffix == ".json"
    }
    try:
        vocabulary = (root / ARTIFACTS["vocabulary"]).read_text(encoding="utf-8")
    except OSError as exc:
        raise ValidationError(f"cannot load {root / ARTIFACTS['vocabulary']}: {exc}") from exc

    matrix = documents["opcodes"]
    source_root = root if (root / "Zend/zend_vm_opcodes.h").is_file() else ROOT
    try:
        generated_matrix = OPCODE_VALIDATOR.build_matrix(
            source_root,
            source_root / "Zend/zend_vm_opcodes.h",
            source_root / "Zend/zend_vm_def.h",
            source_root / "docs/native-engine/semantics/opcodes/opcode-overrides.json",
        )
    except OPCODE_VALIDATOR.MatrixError as exc:
        raise ValidationError(f"opcode validation failed: {exc}") from exc
    require(matrix == generated_matrix, "opcode matrix differs from generator output")

    try:
        EFFECT_VALIDATOR.validate_model(documents["effects"], documents["effects_schema"])
    except EFFECT_VALIDATOR.ValidationError as exc:
        raise ValidationError(f"effect validation failed: {exc}") from exc

    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        frame_status = FRAME_VALIDATOR.check(
            root / ARTIFACTS["frame_schema"], root / ARTIFACTS["frame_examples"]
        )
    require(frame_status == 0, "frame validation failed")

    tpde_errors = TPDE_VALIDATOR.validate_document(documents["tpde"])
    require(not tpde_errors, f"TPDE validation failed: {tpde_errors}")

    try:
        MANIFEST_VALIDATOR.validate_document(documents["manifest"], source_root)
    except MANIFEST_VALIDATOR.ManifestError as exc:
        raise ValidationError(f"manifest validation failed: {exc}") from exc

    require(matrix.get("format_version") == "1.0.0", "opcode matrix format_version must be 1.0.0")
    require(
        matrix.get("source_commit") == OPCODE_SOURCE_COMMIT,
        "opcode matrix source commit is stale",
    )
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
        absent_from_contract = sorted(identifier for identifier in required if f"`{identifier}`" not in vocabulary)
        require(not absent_from_contract, f"semantic vocabulary omits {label}: {absent_from_contract}")

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

    blocked_capabilities: list[str] = []
    for capability in tpde["capabilities"]:
        require(isinstance(capability.get("source_refs"), list) and capability["source_refs"], f"TPDE capability {capability['id']} lacks source refs")
        if capability.get("critical") is True and capability.get("classification") == "blocked":
            blocked_capabilities.append(capability["id"])
    require(not blocked_capabilities, f"critical TPDE capabilities remain blocked: {sorted(blocked_capabilities)}")

    for artifact_name, document in sorted(documents.items()):
        for path, value in walk_strings(document):
            require(PLACEHOLDER.search(value) is None, f"{artifact_name}{path} contains a semantic placeholder")

    return {
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
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--root", type=Path, default=ROOT, help="repository-like artifact root used by contract tests")
    args = parser.parse_args(argv)
    if not args.check:
        parser.error("only --check is supported")
    try:
        report = build_report(args.root.resolve())
    except ValidationError as exc:
        print(f"W01 validation failed: {exc}", file=sys.stderr)
        return 1
    print(f"W01 semantic contracts pass ({report['opcode_coverage']['catalogued']} opcodes, {len(report['test_family_coverage'])} families)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
