#!/usr/bin/env python3
"""Validate the complete frozen W06 value/reference contract."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
MIR = ROOT / "Zend/Native/MIR"
LOWERING = ROOT / "Zend/Native/Lowering"
VALUES = ROOT / "Zend/Native/Values"
DOCS = ROOT / "docs/native-engine/values"
REGISTRY = ROOT / "docs/native-engine/roadmap/capability-registry.json"
GENERATOR = ROOT / "scripts/native/values/generate-w06-profile.py"
HISTORY = ROOT / "scripts/native/values/check-history.py"
PROFILE = DOCS / "w06-opcode-profile.json"
RECLASSIFICATION = DOCS / "w06-reclassification.json"
TRANSITIONS = DOCS / "w06-transition-profile.json"
MANIFEST = DOCS / "w06-phase-manifest.json"

HEADERS = (
    MIR / "zend_mir_ids.h",
    MIR / "zend_mir_opcodes.h",
    MIR / "zend_mir_values.h",
    MIR / "zend_mir_capability.h",
    MIR / "zend_mir_verification.h",
    VALUES / "Contracts/zend_mir_value_source.h",
    VALUES / "Contracts/zend_mir_value_plan.h",
    LOWERING / "zend_mir_lowering.h",
    LOWERING / "zend_mir_lowering_diagnostic.h",
    LOWERING / "zend_mir_lowering_zend.h",
)
POINTER_FREE = (
    "zend_mir_storage_ref",
    "zend_mir_payload_ref",
    "zend_mir_reference_cell_ref",
    "zend_mir_alias_relation_ref",
    "zend_mir_ownership_event_ref",
    "zend_mir_separation_plan_ref",
    "zend_mir_parameter_mode_ref",
    "zend_mir_call_transfer_ref",
    "zend_mir_value_verifier_receipt_ref",
    "zend_mir_source_storage_ref",
    "zend_mir_source_reference_ref",
    "zend_mir_source_indirect_ref",
    "zend_mir_value_plan_entry",
)
CAPABILITIES = {
    "zval_storage_model": 19,
    "reference_cell_model": 20,
    "indirect_slot_model": 21,
    "refcount_transfer_model": 22,
    "alias_partition_model": 23,
    "separation_protocol_model": 24,
    "direct_user_call_reference_transfer_model": 25,
    "refcounted_call_result_model": 26,
}
NEW_DEBTS = {
    "container_clone_execution": 1009,
    "string_and_array_operation_semantics": 1010,
    "object_lifecycle": 1011,
    "destructor_exception_cleanup": 1012,
    "runtime_reference_binding": 1013,
    "dynamic_symbol_table_aliasing": 1014,
}


class ContractError(RuntimeError):
    pass


def run(command: list[str]) -> None:
    env = os.environ.copy()
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    completed = subprocess.run(
        command, cwd=ROOT, env=env, text=True, capture_output=True, check=False
    )
    if completed.returncode:
        output = "\n".join(
            part.rstrip() for part in (completed.stdout, completed.stderr) if part
        )
        raise ContractError(f"command failed ({' '.join(command)}):\n{output}")


def load_generator():
    spec = importlib.util.spec_from_file_location("w06_generator", GENERATOR)
    if spec is None or spec.loader is None:
        raise ContractError("cannot load W06 generator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def struct_body(text: str, name: str) -> str:
    match = re.search(
        rf"typedef\s+struct\s+_{re.escape(name)}\s*\{{(?P<body>.*?)\}}"
        rf"\s*{re.escape(name)}\s*;",
        text,
        re.DOTALL,
    )
    if match is None:
        raise ContractError(f"missing contract record {name}")
    return match.group("body")


def validate_profile() -> None:
    generator = load_generator()
    expected_profile, expected_changes = generator.build()
    profile = json.loads(PROFILE.read_text(encoding="utf-8"))
    changes = json.loads(RECLASSIFICATION.read_text(encoding="utf-8"))
    if profile != expected_profile or changes != expected_changes:
        raise ContractError("generated W06 profile or reclassification drift")
    if profile["active_opcode_count"] != len(profile["opcodes"]):
        raise ContractError("live opcode count drift")
    if profile["unresolved_w06_count"] != 0:
        raise ContractError("unresolved W06 count is nonzero")
    if profile["capabilities_provided"] != generator.CAPABILITIES_PROVIDED:
        raise ContractError("W06 capability output drift")
    if profile["semantic_debts_remaining"] != generator.SEMANTIC_DEBTS_REMAINING:
        raise ContractError("W06 semantic-debt output drift")
    if profile["codegen_eligible"] is not False:
        raise ContractError("W06 must remain non-codegen-eligible")
    if any(entry["later_wave"] == "W06" for entry in profile["opcodes"]):
        raise ContractError("profile retains a W06 deferral")
    live = [entry for entry in profile["opcodes"] if entry["w05_deferred_wave"] == "W06"]
    if len(live) != profile["live_w06_deferred_count"]:
        raise ContractError("live W06 inventory count drift")
    if {entry["opcode"] for entry in live} != (
        set(generator.ACCEPTED) | set(generator.LATER) | set(generator.OWNER_SEQUENCE)
    ):
        raise ContractError("live W06 inventory is not individually decided")
    required_variants = {"ZEND_QM_ASSIGN", "ZEND_FREE", "ZEND_RETURN"}
    variants = {
        entry["opcode"] for entry in profile["opcodes"]
        if entry["decision"] == "accepted_variant"
    }
    if not required_variants <= variants:
        raise ContractError("required refcounted accepted variants are absent")


def validate_schemas() -> None:
    pairs = (
        (PROFILE, DOCS / "w06-opcode-profile.schema.json"),
        (TRANSITIONS, DOCS / "w06-transition-profile.schema.json"),
        (MANIFEST, DOCS / "w06-phase-manifest.schema.json"),
        (REGISTRY, ROOT / "docs/native-engine/roadmap/capability-registry.schema.json"),
    )
    for instance_path, schema_path in pairs:
        instance = json.loads(instance_path.read_text(encoding="utf-8"))
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
        if schema.get("$schema") != "https://json-schema.org/draft/2020-12/schema":
            raise ContractError(f"{schema_path.name}: schema draft drift")
        required = set(schema.get("required", []))
        if not required <= set(instance):
            raise ContractError(f"{instance_path.name}: required properties absent")
        if schema.get("additionalProperties") is False:
            extra = set(instance) - set(schema.get("properties", {}))
            if extra:
                raise ContractError(f"{instance_path.name}: unknown properties {sorted(extra)}")


def validate_headers() -> None:
    texts = {path: path.read_text(encoding="utf-8") for path in HEADERS}
    combined = "\n".join(texts.values())
    ids = texts[MIR / "zend_mir_ids.h"]
    for symbol, value in (
        ("ZEND_MIR_CONTRACT_VERSION_MINOR", 2),
        ("ZEND_MIR_W04_CONTRACT_VERSION_MINOR", 3),
        ("ZEND_MIR_W05_CONTRACT_VERSION_MINOR", 8),
        ("ZEND_MIR_W06_CONTRACT_VERSION_MINOR", 9),
    ):
        if re.search(rf"\b{symbol}\s+UINT32_C\({value}\)", ids) is None:
            raise ContractError(f"contract minor identity drift: {symbol}")
    for name in (
        "zend_mir_storage_id", "zend_mir_payload_id", "zend_mir_reference_cell_id",
        "zend_mir_alias_class_id", "zend_mir_ownership_event_id",
        "zend_mir_separation_plan_id", "zend_mir_parameter_mode_id",
        "zend_mir_verifier_receipt_id",
    ):
        if f"typedef uint32_t {name};" not in ids:
            raise ContractError(f"missing stable 32-bit ID: {name}")
    opcodes = texts[MIR / "zend_mir_opcodes.h"]
    expected = (
        'X(STORAGE_BIND, "storage_bind", 42)',
        'X(REFERENCE_BIND, "reference_bind", 43)',
        'X(INDIRECT_BIND, "indirect_bind", 44)',
        'X(OWNERSHIP_TRANSFER, "ownership_transfer", 45)',
        'X(ALIAS_RELATION, "alias_relation", 46)',
        'X(SEPARATION_PLAN, "separation_plan", 47)',
    )
    if not all(item in opcodes for item in expected):
        raise ContractError("W06 opcode identity drift")
    if "ZEND_MIR_OPCODE_COUNT = 41" not in opcodes or "ZEND_MIR_W05_OPCODE_COUNT = 42" not in opcodes:
        raise ContractError("W03/W05 opcode boundary changed")
    if "ZEND_MIR_W06_OPCODE_COUNT = 48" not in opcodes:
        raise ContractError("W06 opcode boundary drift")
    for record in POINTER_FREE:
        if "*" in struct_body(combined, record):
            raise ContractError(f"raw pointer in persistent W06 record: {record}")
    if re.search(r"\b(?:numeric_)?refcount\s*;", combined):
        raise ContractError("numeric refcount leaked into W06 records")
    if "ZEND_MIR_VERIFIER_VALUE_REFERENCE = 5" not in combined:
        raise ContractError("value/reference verifier ID drift")
    for number in range(30, 38):
        if f"[MIRL{number:04d}]" not in combined:
            raise ContractError(f"missing stable MIRL token {number:04d}")
    for number in range(800, 808):
        if f"[MIRV{number:04d}]" not in combined:
            raise ContractError(f"missing stable MIRV token {number:04d}")


def validate_registry() -> None:
    data = json.loads(REGISTRY.read_text(encoding="utf-8"))
    entries = data["entries"]
    identities = [(entry["kind"], entry["id"]) for entry in entries]
    transports = [entry["transport_id"] for entry in entries]
    if len(identities) != len(set(identities)) or len(transports) != len(set(transports)):
        raise ContractError("capability registry IDs are not unique")
    by_identity = {(entry["kind"], entry["id"]): entry for entry in entries}
    for name, transport in {**CAPABILITIES, **NEW_DEBTS}.items():
        kind = "capability" if name in CAPABILITIES else "semantic_debt"
        if by_identity.get((kind, name), {}).get("transport_id") != transport:
            raise ContractError(f"registry transport ID drift: {name}")
    if by_identity[("capability", "target_emission")]["transport_id"] != 15:
        raise ContractError("existing target_emission capability changed")
    if by_identity[("capability", "internal_c_abi_interop")]["transport_id"] != 18:
        raise ContractError("existing internal_c_abi_interop capability changed")
    known = {entry["id"] for entry in entries}
    for entry in entries:
        refs = entry["prerequisites"] + entry["closes_debts"] + entry["incompatible_with"]
        if not set(refs) <= known:
            raise ContractError(f"{entry['id']}: unknown registry reference")


def validate_transitions() -> None:
    data = json.loads(TRANSITIONS.read_text(encoding="utf-8"))
    actions = [entry["action"] for entry in data["transitions"]]
    expected = ["borrow", "copy_addref", "move", "release", "transfer_to_callee", "transfer_from_callee"]
    if actions != expected:
        raise ContractError("ownership transition order drift")
    if data["alias_merge"]["unknown_or_conflicting"] != "may_alias":
        raise ContractError("unknown alias state is not conservative")
    if data["separation"]["clone_execution"] is not False:
        raise ContractError("separation contract executes a clone")
    if data["call_transfer"]["parameter_mode_storage"] != "span":
        raise ContractError("parameter modes are not scalable")
    if data["call_transfer"]["parameter_ordinal_max"] < 128:
        raise ContractError("parameter mode model cannot represent ordinal 128")


def compile_headers() -> None:
    cc = shutil.which(os.environ.get("CC", "cc"))
    cxx = shutil.which(os.environ.get("CXX", "c++"))
    if cc is None or cxx is None:
        raise ContractError("C11/C++20 compiler unavailable")
    source = '#include "Zend/Native/Lowering/zend_mir_lowering_zend.h"\nint main(void) { return 0; }\n'
    with tempfile.TemporaryDirectory(prefix="w06-contract-") as directory:
        path = Path(directory) / "contract.c"
        path.write_text(source, encoding="utf-8")
        run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I.", "-fsyntax-only", str(path)])
        cpp = Path(directory) / "contract.cpp"
        cpp.write_text(source, encoding="utf-8")
        run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I.", "-fsyntax-only", str(cpp)])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if not args.check:
        parser.error("--check is required")
    try:
        validate_profile()
        validate_schemas()
        validate_headers()
        validate_registry()
        validate_transitions()
        compile_headers()
        run([sys.executable, str(GENERATOR), "--check"])
    except (OSError, json.JSONDecodeError, KeyError, ContractError, ValueError) as exc:
        print(f"W06 contract invalid: {exc}", file=sys.stderr)
        return 1
    print("W06 contract valid: profiles, IDs, headers, transitions, registry and compilers")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
