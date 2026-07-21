#!/usr/bin/env python3
"""Validate the complete frozen W05 direct-user-call modeling contract."""

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
from typing import Any


sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[3]
CALLS = ROOT / "Zend/Native/Calls"
MIR = ROOT / "Zend/Native/MIR"
LOWERING = ROOT / "Zend/Native/Lowering"
DOCS = ROOT / "docs/native-engine/calls"
FIXTURES = ROOT / "tests/native/calls/contracts"
PROFILE = DOCS / "w05-opcode-profile.json"
SEQUENCE = DOCS / "w05-sequence-profile.json"
RECLASSIFICATION = DOCS / "w05-reclassification.json"
GENERATOR = ROOT / "scripts/native/calls/generate-w05-profile.py"

HEADERS = (
    MIR / "zend_mir_ids.h",
    MIR / "zend_mir_opcodes.h",
    CALLS / "Contracts/zend_mir_call_source.h",
    CALLS / "Contracts/zend_mir_call_plan.h",
    MIR / "zend_mir_call.h",
    LOWERING / "zend_mir_lowering.h",
    LOWERING / "zend_mir_lowering_diagnostic.h",
    LOWERING / "zend_mir_lowering_zend.h",
)
SCHEMA_PAIRS = ((PROFILE, DOCS / "w05-opcode-profile.schema.json"),)
POINTER_FREE_RECORDS = (
    "zend_mir_source_call_site_ref",
    "zend_mir_source_call_target_ref",
    "zend_mir_source_call_argument_ref",
    "zend_mir_source_parameter_mode_ref",
    "zend_mir_call_target_ref",
    "zend_mir_call_argument_ref",
    "zend_mir_call_frame_descriptor",
    "zend_mir_call_continuation_ref",
    "zend_mir_call_site_ref",
)
CAPABILITIES = (
    "scalar_semantics",
    "reducible_control_flow",
    "direct_user_call_sequence",
    "caller_frame_descriptor",
    "callee_entry_descriptor",
    "abstract_call_effects",
)
DEBTS = (
    "call_execution",
    "exception_cleanup",
    "refcounted_transfer",
    "protected_continuation",
    "dynamic_target_resolution",
    "observer_interop",
    "cow_indirect_semantics",
    "internal_c_abi_interop",
)


class ContractError(RuntimeError):
    """The checked-in W05 contract is inconsistent."""


def run(command: list[str]) -> None:
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        output = "\n".join(
            part.rstrip() for part in (completed.stdout, completed.stderr) if part
        )
        raise ContractError(f"command failed ({' '.join(command)}):\n{output}")


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise ContractError(f"cannot load {path.relative_to(ROOT)}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def extract_struct(text: str, name: str) -> str:
    match = re.search(
        rf"typedef\s+struct\s+_{re.escape(name)}\s*\{{(?P<body>.*?)\}}"
        rf"\s*{re.escape(name)}\s*;",
        text,
        re.DOTALL,
    )
    if match is None:
        raise ContractError(f"missing contract record {name}")
    return match.group("body")


def validate_schema_documents() -> None:
    required_sets = {
        PROFILE: {
            "active_opcode_count",
            "call_adjacent_opcode_count",
            "format_version",
            "mir_contract_version",
            "opcodes",
            "proof_catalog",
            "reserved_opcode_numbers",
            "sources",
            "unresolved_w05_count",
        },
    }
    for instance_path, schema_path in SCHEMA_PAIRS:
        instance = json.loads(instance_path.read_text(encoding="utf-8"))
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
        if schema.get("$schema") != "https://json-schema.org/draft/2020-12/schema":
            raise ContractError(f"{schema_path.name}: schema draft drift")
        if set(schema.get("required", [])) != required_sets[instance_path]:
            raise ContractError(f"{schema_path.name}: frozen required set drift")
        missing = required_sets[instance_path] - set(instance)
        extra = set(instance) - set(schema.get("properties", {}))
        if missing or extra:
            raise ContractError(
                f"{instance_path.name}: schema shape mismatch "
                f"(missing={sorted(missing)}, extra={sorted(extra)})"
            )


def validate_profile() -> None:
    generator = load_module("w05_profile_generator", GENERATOR)
    generated_profile, generated_reclassification = generator.build()
    profile = json.loads(PROFILE.read_text(encoding="utf-8"))
    reclassification = json.loads(RECLASSIFICATION.read_text(encoding="utf-8"))
    if (
        profile != generated_profile
        or reclassification != generated_reclassification
    ):
        raise ContractError("W05 generated profile or reclassification drift")
    if profile["active_opcode_count"] != len(profile["opcodes"]):
        raise ContractError("live opcode count does not match profile entries")
    if profile["unresolved_w05_count"] != 0:
        raise ContractError("unresolved W05 deferral remains")
    if any(entry.get("deferred_wave") == "W05" for entry in profile["opcodes"]):
        raise ContractError("profile contains an unresolved W05 opcode")
    adjacent = [entry for entry in profile["opcodes"] if generator.is_call_adjacent(entry)]
    if len(adjacent) != profile["call_adjacent_opcode_count"]:
        raise ContractError("call-adjacent live count drift")
    decided = set(generator.ACCEPTED) | set(generator.LATER)
    undecided = {entry["opcode"] for entry in adjacent} - decided
    if undecided:
        raise ContractError(f"call-adjacent opcodes lack individual decisions: {sorted(undecided)}")
    for entry in adjacent:
        if entry["opcode"] in generator.ACCEPTED:
            if entry["classification"] != "conditional":
                raise ContractError(f"{entry['opcode']}: accepted fragment is not conditional")
            if entry["proofs"] != generator.CALL_PROOFS:
                raise ContractError(f"{entry['opcode']}: call-sequence proof set drift")
            if "never published independently" not in entry["rationale"]:
                raise ContractError(f"{entry['opcode']}: fragment publication is not forbidden")
        elif entry["classification"] != "deferred" or entry["deferred_wave"] is None:
            raise ContractError(f"{entry['opcode']}: later-wave decision is incomplete")


def validate_headers() -> None:
    texts = {path: path.read_text(encoding="utf-8") for path in HEADERS}
    combined = "\n".join(texts.values())
    ids = texts[MIR / "zend_mir_ids.h"]
    versions = (
        ("ZEND_MIR_CONTRACT_VERSION_MINOR", 2),
        ("ZEND_MIR_W04_CONTRACT_VERSION_MINOR", 3),
        ("ZEND_MIR_W05_CONTRACT_VERSION_MINOR", 9),
    )
    for symbol, value in versions:
        if re.search(rf"\b{symbol}\s+UINT32_C\({value}\)", ids) is None:
            raise ContractError(f"contract minor identity drift: {symbol}")
    opcodes = texts[MIR / "zend_mir_opcodes.h"]
    for value in range(41):
        if re.search(rf",\s*{value}\)", opcodes) is None:
            raise ContractError(f"W01-W04 MIR opcode identity missing: {value}")
    if 'X(CALL_DIRECT_USER, "call_direct_user", 41)' not in opcodes:
        raise ContractError("CALL_DIRECT_USER is not stable opcode 41")
    if "ZEND_MIR_OPCODE_COUNT = 41" not in opcodes:
        raise ContractError("frozen W03 scalar opcode boundary changed")
    if "ZEND_MIR_W05_OPCODE_COUNT = 42" not in opcodes:
        raise ContractError("W05 opcode table boundary is not additive")

    for record in POINTER_FREE_RECORDS:
        if "*" in extract_struct(combined, record):
            raise ContractError(f"raw pointer in immutable W05 record: {record}")
    source_view = extract_struct(combined, "zend_mir_source_call_view")
    source_order = (
        "call_site_count",
        "call_site_at",
        "call_target_count",
        "call_target_at",
        "call_argument_count",
        "call_argument_at",
        "parameter_mode_count",
        "parameter_mode_at",
        "source_opcode_count",
        "source_opcode_at",
    )
    positions = [source_view.find(token) for token in source_order]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        raise ContractError("source call tables are absent or out of semantic order")
    target = extract_struct(combined, "zend_mir_source_call_target_ref")
    if "zend_mir_span parameter_modes;" not in target or "by_ref_mask" in target:
        raise ContractError("source call target lacks scalable parameter modes")
    if "ZEND_MIR_ZEND_SEND_SYNTACTIC_NAMED" in combined:
        raise ContractError("compiler named-syntax side channel remains in contract")

    required = (
        "zend_mir_source_call_site_id",
        "zend_mir_source_call_argument_id",
        "zend_mir_source_call_target_id",
        "zend_mir_call_plan",
        "zend_mir_call_plan_decision",
        "zend_mir_verify_w05_calls",
        "zend_mir_lower_w05_zend_source",
        "ZEND_MIR_W05_REQUIRED_CAPABILITIES",
        "ZEND_MIR_W05_REQUIRED_DEBTS",
        "zend_mir_lowering_result_is_w05_failure_atomic",
        "prerequisite_guarantees",
        "function_symbol_id",
    )
    for token in required:
        if token not in combined:
            raise ContractError(f"missing W05 contract token: {token}")
    if "STAGE4" in combined.upper():
        raise ContractError("W05 introduced forbidden Stage 4 semantics")
    diagnostics = texts[LOWERING / "zend_mir_lowering_diagnostic.h"]
    for value in range(21, 30):
        if re.search(rf"=\s*{value}\b", diagnostics) is None:
            raise ContractError(f"missing stable MIRL diagnostic {value:04d}")
        if f"[MIRL{value:04d}]" not in diagnostics:
            raise ContractError(f"missing MIRL token {value:04d}")
    verifier = texts[MIR / "zend_mir_call.h"]
    for value in range(700, 706):
        if re.search(rf"=\s*{value}\b", verifier) is None:
            raise ContractError(f"missing stable MIRV diagnostic {value:04d}")
        if f"[MIRV{value:04d}]" not in verifier:
            raise ContractError(f"missing MIRV token {value:04d}")

    lowering = texts[LOWERING / "zend_mir_lowering.h"]
    if not re.search(
        r"ZEND_MIR_LOWERING_GUARANTEE_ALL\s*=\s*"
        r"ZEND_MIR_LOWERING_GUARANTEE_W03_ALL",
        lowering,
    ):
        raise ContractError("W03 guarantee alias changed")
    if (
        "zend_mir_lowering_result_is_w04_failure_atomic" not in lowering
        or "ZEND_MIR_LOWERING_GUARANTEE_W04_ALL" not in lowering
    ):
        raise ContractError("W04 guarantee/result compatibility was removed")
    w05_result = extract_struct(combined, "zend_mir_w05_lowering_result")
    if "prerequisite_guarantees" not in w05_result:
        raise ContractError("W05 prerequisite guarantees are absent")
    if (
        "result->lowering.guarantees\n\t\t\t\t== ZEND_MIR_LOWERING_GUARANTEE_FINALIZED"
        not in lowering
        or "result->prerequisite_guarantees\n\t\t\t\t== ZEND_MIR_LOWERING_GUARANTEE_W04_ALL"
        not in lowering
    ):
        raise ContractError("W05 final/prerequisite guarantee split drift")
    frame = extract_struct(combined, "zend_mir_call_frame_descriptor")
    if "function_symbol_id" not in frame:
        raise ContractError("W05 frame lacks stable logical function identity")
    call_site = extract_struct(combined, "zend_mir_call_site_ref")
    if (
        "zend_mir_value_id result_id;" not in call_site
        or call_site.find("result_id") < call_site.find("arguments")
    ):
        raise ContractError("W05 call site lacks the ordered scalar result identity")
    sequence_contract = (DOCS / "contracts/call-sequence.md").read_text(
        encoding="utf-8"
    )
    for token in ("result_id", "source-opline order", "source result SSA"):
        if token not in sequence_contract:
            raise ContractError(f"W05 result/order contract lacks {token}")
    forbidden = re.search(
        r"#\s*include[^\n]*(?:tpde|x86|aarch64|arm64|riscv|zend_vm|zend_jit)"
        r"|\b(?:zif_handler|zend_execute|execute_ex|mir_interpret|mir_evaluate)\b",
        combined,
        re.IGNORECASE,
    )
    if forbidden:
        raise ContractError(f"runtime/target contract leak: {forbidden.group(0)}")


def validate_sequence_profile() -> None:
    sequence = json.loads(SEQUENCE.read_text(encoding="utf-8"))
    if tuple(sequence["capabilities"]) != CAPABILITIES:
        raise ContractError("capability list drift")
    if tuple(sequence["debts"]) != DEBTS:
        raise ContractError("semantic debt list drift")
    if not sequence["modeled"] or sequence["codegen_eligible"]:
        raise ContractError("W05 call model must be modeled and not codegen eligible")
    grammar = sequence["accepted_grammar"]
    if grammar["init"]["opcode"] != "ZEND_INIT_FCALL":
        raise ContractError("accepted call sequence does not start with INIT_FCALL")
    if set(grammar["finish"]["opcodes"]) != {"ZEND_DO_UCALL", "ZEND_DO_FCALL"}:
        raise ContractError("accepted call sequence finish set drift")
    if grammar["nesting"] != "balanced_lifo_init_send_do":
        raise ContractError("nested call stack ordering is not frozen")
    rejection_cases = {item["case"]: item["diagnostic"] for item in sequence["rejections"]}
    required_cases = {
        "orphan_send_or_do": "MIRL0022",
        "malformed_or_unbalanced_nesting": "MIRL0021",
        "dynamic_method_or_internal_target": "MIRL0023",
        "by_ref_named_unpack_variadic_placeholder_or_refcounted_argument": "MIRL0024",
        "argument_count_mismatch_or_default": "MIRL0025",
        "unproved_or_refcounted_result": "MIRL0026",
        "protected_region": "MIRL0027",
        "allocation_or_plan_publication_failure": "MIRL0028",
    }
    if rejection_cases != required_cases:
        raise ContractError("W05 rejection/diagnostic matrix drift")


def validate_fixtures() -> None:
    fixture = json.loads(
        (FIXTURES / "call-sequence-fixtures.json").read_text(encoding="utf-8")
    )
    cases = {case["name"]: case for case in fixture["cases"]}
    expected = {
        "simple-direct-user",
        "nested-direct-user",
        "orphan-send",
        "orphan-do",
        "dynamic-target",
        "internal-target",
        "by-reference",
        "named-argument",
        "named-argument-normalized-position",
        "unpack",
        "default-argument",
        "default-fully-supplied",
        "exact-self-call",
        "parameter-mode-boundaries",
        "refcounted-result",
        "protected-call",
        "allocation-failure",
    }
    if set(cases) != expected:
        raise ContractError("call-sequence fixture set drift")
    for name in (
        "simple-direct-user",
        "nested-direct-user",
        "named-argument-normalized-position",
        "default-fully-supplied",
        "exact-self-call",
        "parameter-mode-boundaries",
    ):
        if cases[name]["decision"] != "accepted" or not cases[name]["atomic"]:
            raise ContractError(f"{name}: valid sequence is not atomically accepted")
    for name, case in cases.items():
        if case["decision"] != "accepted" and not case.get("diagnostic"):
            raise ContractError(f"{name}: negative fixture lacks stable diagnostic")
    normalized_named = cases["named-argument-normalized-position"]
    if (
        not normalized_named.get("normalized_to_position")
        or normalized_named.get("decision") != "accepted"
    ):
        raise ContractError("normalized named call is not accepted")
    boundaries = cases["parameter-mode-boundaries"]
    if boundaries.get("ordinals") != [0, 63, 64, 127]:
        raise ContractError("parameter mode boundary fixture drift")
    nested = cases["nested-direct-user"]["events"]
    stack: list[int] = []
    for event in nested:
        if event["kind"] == "INIT":
            stack.append(event["site"])
        elif event["kind"] == "SEND":
            if not stack or stack[-1] != event["site"]:
                raise ContractError("nested SEND is not assigned to active LIFO site")
        elif event["kind"] == "DO":
            if not stack or stack.pop() != event["site"]:
                raise ContractError("nested DO does not close the active LIFO site")
    if stack:
        raise ContractError("nested fixture leaves an incomplete call site")


def compiler(default: str, variable: str) -> str:
    selected = os.environ.get(variable, default)
    resolved = shutil.which(selected)
    if resolved is None:
        raise ContractError(f"required compiler not found: {selected}")
    return resolved


def compile_contract() -> None:
    cc = compiler("cc", "CC")
    cxx = compiler("c++", "CXX")
    warnings = ["-pedantic-errors", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT)]
    with tempfile.TemporaryDirectory(prefix="w05-contract-") as temporary:
        temp = Path(temporary)
        for index, header in enumerate(HEADERS):
            relative = header.relative_to(ROOT).as_posix()
            body = f'#include "{relative}"\nint main(void) {{ return 0; }}\n'
            c_source = temp / f"header-{index}.c"
            cxx_source = temp / f"header-{index}.cc"
            c_source.write_text(body, encoding="utf-8")
            cxx_source.write_text(body, encoding="utf-8")
            run([cc, "-std=c11", *warnings, "-c", str(c_source), "-o", str(temp / f"{index}.o")])
            run([cxx, "-std=c++20", *warnings, "-c", str(cxx_source), "-o", str(temp / f"{index}.opp")])
        fixture = FIXTURES / "test_contract.c"
        c_binary = temp / "contract-c"
        cxx_binary = temp / "contract-cxx"
        run([cc, "-std=c11", *warnings, str(fixture), "-o", str(c_binary)])
        run([str(c_binary)])
        run([cxx, "-std=c++20", "-x", "c++", *warnings, str(fixture), "-o", str(cxx_binary)])
        run([str(cxx_binary)])


def check() -> None:
    validate_schema_documents()
    validate_profile()
    validate_headers()
    validate_sequence_profile()
    validate_fixtures()
    compile_contract()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if not args.check:
        parser.error("only --check is supported")
    try:
        check()
    except (ContractError, OSError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"W05 call contract check failed: {error}", file=sys.stderr)
        return 1
    print(
        "W05 call contract check passed: C11/C++20, profile, fixtures, "
        "capabilities and debts"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
