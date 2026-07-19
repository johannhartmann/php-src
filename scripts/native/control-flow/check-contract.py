#!/usr/bin/env python3
"""Validate the complete frozen W04 source/control-flow contract."""

from __future__ import annotations

import argparse
import hashlib
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
LOWERING = ROOT / "Zend/Native/Lowering"
MIR = ROOT / "Zend/Native/MIR"
FIXTURES = ROOT / "tests/native/control-flow/contracts"
PROFILE = ROOT / "docs/native-engine/control-flow/w04-opcode-profile.json"
MATRIX = ROOT / "docs/native-engine/semantics/opcodes/opcode-matrix.json"
W03_PROFILE = ROOT / "docs/native-engine/lowering/w03-opcode-profile.json"
OWNERSHIP = ROOT / "docs/native-engine/control-flow/w04-ownership.json"
SOURCE_FILES = ROOT / "docs/native-engine/control-flow/w04-source-files.json"
BLOCKERS = ROOT / "docs/native-engine/control-flow/w04-blockers.json"
GENERATOR = ROOT / "scripts/native/control-flow/generate-w04-profile.py"
OWNERSHIP_CHECKER = ROOT / "scripts/native/control-flow/check-ownership.py"

HEADERS = (
    MIR / "zend_mir_ids.h",
    LOWERING / "zend_mir_lowering_source.h",
    LOWERING / "zend_mir_lowering_diagnostic.h",
    LOWERING / "zend_mir_lowering.h",
    LOWERING / "zend_mir_control_flow.h",
    MIR / "zend_mir_control_flow.h",
    LOWERING / "zend_mir_lowering_zend.h",
    MIR / "Verify/zend_mir_verify_control_flow.h",
)

SCHEMA_PAIRS = (
    (PROFILE, ROOT / "docs/native-engine/control-flow/w04-opcode-profile.schema.json"),
    (OWNERSHIP, ROOT / "docs/native-engine/control-flow/w04-ownership.schema.json"),
    (SOURCE_FILES, ROOT / "docs/native-engine/control-flow/w04-source-files.schema.json"),
    (BLOCKERS, ROOT / "docs/native-engine/control-flow/w04-blockers.schema.json"),
)

POINTER_FREE_RECORDS = (
    "zend_mir_source_block_ref",
    "zend_mir_source_edge_ref",
    "zend_mir_source_phi_constraint",
    "zend_mir_source_phi_ref",
    "zend_mir_source_phi_input_ref",
    "zend_mir_control_flow_block_mapping",
    "zend_mir_control_flow_edge_mapping",
    "zend_mir_control_flow_phi_mapping",
)


class ContractError(RuntimeError):
    """The checked-in W04 contract is inconsistent."""


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
    expected_required = {
        PROFILE: {
            "format_version",
            "mir_contract_version",
            "sources",
            "active_opcode_count",
            "reserved_opcode_numbers",
            "proof_catalog",
            "opcodes",
        },
        OWNERSHIP: {
            "format_version",
            "wave",
            "specialist_tasks",
            "integration_task",
            "contract_reserved_paths",
        },
        SOURCE_FILES: {
            "format_version",
            "consumer",
            "existing_production_sources",
            "w04_production_sources",
        },
        BLOCKERS: {
            "format_version",
            "wave",
            "open_blockers",
            "resolved_w04_deferrals",
        },
    }
    for instance_path, schema_path in SCHEMA_PAIRS:
        instance = json.loads(instance_path.read_text(encoding="utf-8"))
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
        if schema.get("$schema") != "https://json-schema.org/draft/2020-12/schema":
            raise ContractError(f"{schema_path.name}: schema draft drift")
        required = set(schema.get("required", []))
        if required != expected_required[instance_path]:
            raise ContractError(f"{schema_path.name}: frozen root required set drift")
        missing = required - set(instance)
        extra = set(instance) - set(schema.get("properties", {}))
        if missing or extra:
            raise ContractError(
                f"{instance_path.name}: schema shape mismatch "
                f"(missing={sorted(missing)}, extra={sorted(extra)})"
            )


def validate_profile() -> None:
    generator = load_module("w04_profile_generator", GENERATOR)
    matrix = json.loads(MATRIX.read_text(encoding="utf-8"))
    w03 = json.loads(W03_PROFILE.read_text(encoding="utf-8"))
    profile = json.loads(PROFILE.read_text(encoding="utf-8"))
    errors = generator.validate_profile(
        profile,
        matrix,
        w03,
        matrix_sha256=hashlib.sha256(MATRIX.read_bytes()).hexdigest(),
        w03_sha256=hashlib.sha256(W03_PROFILE.read_bytes()).hexdigest(),
    )
    if errors:
        raise ContractError("; ".join(errors))

    inherited = {
        entry["opcode"]
        for entry in w03["opcodes"]
        if entry["classification"] != "deferred"
    }
    accepted = {
        entry["opcode"]
        for entry in profile["opcodes"]
        if entry["classification"] != "deferred"
    }
    if not inherited.issubset(accepted):
        raise ContractError("W04 dropped a W03-accepted scalar provider")
    if any(
        "single_reachable_block" in entry["proofs"]
        for entry in profile["opcodes"]
    ):
        raise ContractError("W03 single-block proof remains in W04")
    if any(entry.get("deferred_wave") == "W04" for entry in profile["opcodes"]):
        raise ContractError("unresolved W04 opcode deferral remains")

    old_w04 = {
        entry["opcode"]
        for entry in w03["opcodes"]
        if entry.get("deferred_wave") == "W04"
    }
    blockers = json.loads(BLOCKERS.read_text(encoding="utf-8"))
    resolved = {
        entry["opcode"] for entry in blockers["resolved_w04_deferrals"]
    }
    if old_w04 != resolved:
        raise ContractError("blocker decisions do not exactly cover live W03 W04 deferrals")
    if blockers["open_blockers"]:
        raise ContractError("W04 blocker manifest is not closed")


def validate_headers() -> None:
    texts = {path: path.read_text(encoding="utf-8") for path in HEADERS}
    combined = "\n".join(texts.values())
    ids = texts[MIR / "zend_mir_ids.h"]
    if not re.search(r"ZEND_MIR_CONTRACT_VERSION_MINOR UINT32_C\(2\)", ids):
        raise ContractError("W03 MIR contract version changed from 1.2")
    if not re.search(r"ZEND_MIR_W04_CONTRACT_VERSION_MINOR UINT32_C\(3\)", ids):
        raise ContractError("W04 additive contract version is not 1.3")

    diagnostics = texts[LOWERING / "zend_mir_lowering_diagnostic.h"]
    expected_codes = {
        "ZEND_MIRL_OK": 0,
        "ZEND_MIRL_DEFERRED_OPCODE": 1,
        "ZEND_MIRL_MISSING_PROOF": 2,
        "ZEND_MIRL_CONTRADICTORY_FACT": 3,
        "ZEND_MIRL_DUPLICATE_PROVIDER_CLAIM": 4,
        "ZEND_MIRL_UNKNOWN_PROVIDER": 5,
        "ZEND_MIRL_INVALID_SOURCE": 6,
        "ZEND_MIRL_MUTATION_FAILED": 7,
        "ZEND_MIRL_FINALIZE_FAILED": 8,
        "ZEND_MIRL_STAGE1_VERIFY_FAILED": 9,
        "ZEND_MIRL_STAGE2_VERIFY_FAILED": 10,
        "ZEND_MIRL_W04_CONTROL_FLOW_DEFERRED": 11,
        "ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED": 12,
        "ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED": 13,
        "ZEND_MIRL_W04_MALFORMED_CFG": 14,
        "ZEND_MIRL_W04_PROTECTED_REGION": 15,
        "ZEND_MIRL_W04_IRREDUCIBLE_LOOP": 16,
        "ZEND_MIRL_W04_UNSUPPORTED_PHI_PI": 17,
        "ZEND_MIRL_W04_BRANCH_PROOF_FAILED": 18,
        "ZEND_MIRL_W04_SOURCE_MIR_MAPPING_FAILED": 19,
        "ZEND_MIRL_STAGE3_VERIFY_FAILED": 20,
    }
    for symbol, value in expected_codes.items():
        if re.search(rf"\b{symbol}\s*=\s*{value}\b", diagnostics) is None:
            raise ContractError(f"diagnostic identity drift: {symbol}")
        if f"[MIRL{value:04d}]" not in diagnostics:
            raise ContractError(f"diagnostic token missing: MIRL{value:04d}")

    verifier = texts[MIR / "Verify/zend_mir_verify_control_flow.h"]
    for value in range(600, 606):
        if f"[MIRV{value:04d}]" not in verifier:
            raise ContractError(f"verifier token missing: MIRV{value:04d}")
    required = (
        "zend_mir_source_block_id",
        "zend_mir_source_edge_id",
        "zend_mir_source_phi_id",
        "zend_mir_source_block_ref",
        "zend_mir_source_edge_ref",
        "zend_mir_source_phi_ref",
        "zend_mir_source_phi_input_ref",
        "zend_mir_control_flow_map",
        "zend_mir_verify_w04_control_flow",
        "zend_mir_lower_w04_zend_source",
        "ZEND_MIR_LOWERING_GUARANTEE_STAGE3_VERIFIED",
        "ZEND_MIR_LOWERING_GUARANTEE_W03_ALL",
        "ZEND_MIR_LOWERING_GUARANTEE_W04_ALL",
        "zend_mir_lowering_result_is_w04_failure_atomic",
    )
    for token in required:
        if token not in combined:
            raise ContractError(f"missing W04 ABI token: {token}")

    source = texts[LOWERING / "zend_mir_lowering_source.h"]
    opcode_body = extract_struct(source, "zend_mir_source_opcode_ref")
    if opcode_body.rfind("block_id") < opcode_body.rfind("source_position_id"):
        raise ContractError("source opcode block_id was not appended")
    view_body = extract_struct(source, "zend_mir_lowering_source_view")
    callback_order = (
        "block_count",
        "block_at",
        "edge_count",
        "edge_at",
        "phi_count",
        "phi_at",
        "phi_input_count",
        "phi_input_at",
    )
    positions = [view_body.find(token) for token in callback_order]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        raise ContractError("source CFG callbacks are absent or out of semantic order")
    if positions[0] < view_body.find("literal_at"):
        raise ContractError("source CFG callbacks were not appended")

    for record in POINTER_FREE_RECORDS:
        if "*" in extract_struct(combined, record):
            raise ContractError(f"raw pointer in immutable W04 record: {record}")

    lowering = texts[LOWERING / "zend_mir_lowering.h"]
    if not re.search(
        r"ZEND_MIR_LOWERING_GUARANTEE_ALL\s*=\s*"
        r"ZEND_MIR_LOWERING_GUARANTEE_W03_ALL",
        lowering,
    ):
        raise ContractError("W03 guarantee alias changed")
    forbidden = re.search(
        r"#\s*include[^\n]*(?:tpde|x86|aarch64|arm64|riscv|zend_vm|zend_jit)"
        r"|\b(?:zend_op_array|zend_op|zval|zend_ssa)\b",
        combined,
        re.IGNORECASE,
    )
    if forbidden:
        raise ContractError(f"target/runtime type in W04 ABI: {forbidden.group(0)}")


def validate_source_fixture(case: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    blocks = case.get("blocks", [])
    edges = case.get("edges", [])
    phis = case.get("phis", [])
    inputs = case.get("phi_inputs", [])
    block_ids = [block.get("id") for block in blocks]
    edge_ids = [edge.get("id") for edge in edges]
    phi_ids = [phi.get("id") for phi in phis]
    if block_ids != list(range(len(blocks))):
        errors.append("block-id-order")
    if edge_ids != list(range(len(edges))):
        errors.append("edge-id-order")
    if phi_ids != list(range(len(phis))):
        errors.append("phi-id-order")

    predecessors: dict[int, dict[int, int]] = {}
    successors: dict[int, list[int]] = {}
    for edge in edges:
        source = edge["from_block_id"]
        target = edge["to_block_id"]
        successors.setdefault(source, []).append(edge["successor_index"])
        target_predecessors = predecessors.setdefault(target, {})
        index = edge["predecessor_index"]
        if index in target_predecessors:
            errors.append("duplicate-predecessor-index")
        target_predecessors[index] = source
    for values in successors.values():
        if sorted(values) != list(range(len(values))):
            errors.append("successor-index-order")
    for values in predecessors.values():
        if sorted(values) != list(range(len(values))):
            errors.append("predecessor-index-order")

    inputs_by_phi: dict[int, list[dict[str, Any]]] = {}
    for item in inputs:
        inputs_by_phi.setdefault(item["phi_id"], []).append(item)
    for phi in phis:
        phi_inputs = inputs_by_phi.get(phi["id"], [])
        phi_inputs.sort(key=lambda item: item["input_index"])
        if [item["input_index"] for item in phi_inputs] != list(range(len(phi_inputs))):
            errors.append("phi-input-index-order")
        expected = predecessors.get(phi["block_id"], {})
        for item in phi_inputs:
            if expected.get(item["input_index"]) != item["predecessor_block_id"]:
                errors.append("phi-predecessor-order")
    return sorted(set(errors))


def validate_fixtures() -> None:
    document = json.loads(
        (FIXTURES / "source-cfg-fixtures.json").read_text(encoding="utf-8")
    )
    required_names = {
        "diamond",
        "loop",
        "pi-type-and-range",
        "malformed-phi-predecessor-order",
    }
    cases = {case["name"]: case for case in document["cases"]}
    if set(cases) != required_names:
        raise ContractError("source-CFG fixture set drift")
    for case in cases.values():
        errors = validate_source_fixture(case)
        if case["valid"] and errors:
            raise ContractError(f"{case['name']}: unexpected fixture errors {errors}")
        if not case["valid"] and case["expected_error"] not in errors:
            raise ContractError(f"{case['name']}: malformed fixture was accepted")
    loop = cases["loop"]
    if not any(
        "backedge" in edge["flags"] and "interrupt_boundary" in edge["flags"]
        for edge in loop["edges"]
    ):
        raise ContractError("loop fixture lacks source-backed interrupt backedge")


def validate_w03_goldens() -> None:
    manifest = FIXTURES / "w03-golden.sha256"
    for line in manifest.read_text(encoding="utf-8").splitlines():
        expected, relative = line.split(maxsplit=1)
        actual = hashlib.sha256((ROOT / relative).read_bytes()).hexdigest()
        if actual != expected:
            raise ContractError(f"W03 golden bytes changed: {relative}")


def compiler(default: str, variable: str) -> str:
    selected = os.environ.get(variable, default)
    resolved = shutil.which(selected)
    if resolved is None:
        raise ContractError(f"required compiler not found: {selected}")
    return resolved


def compile_contract() -> None:
    cc = compiler("cc", "CC")
    cxx = compiler("c++", "CXX")
    warnings = [
        "-pedantic-errors",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(ROOT),
    ]
    with tempfile.TemporaryDirectory(prefix="w04-contract-") as temporary:
        temp = Path(temporary)
        for index, header in enumerate(HEADERS):
            relative = header.relative_to(ROOT).as_posix()
            body = f'#include "{relative}"\nint main(void) {{ return 0; }}\n'
            c_source = temp / f"header-{index}.c"
            cxx_source = temp / f"header-{index}.cc"
            c_source.write_text(body, encoding="utf-8")
            cxx_source.write_text(body, encoding="utf-8")
            run(
                [
                    cc,
                    "-std=c11",
                    *warnings,
                    "-c",
                    str(c_source),
                    "-o",
                    str(temp / f"header-{index}.o"),
                ]
            )
            run(
                [
                    cxx,
                    "-std=c++20",
                    *warnings,
                    "-c",
                    str(cxx_source),
                    "-o",
                    str(temp / f"header-{index}.opp"),
                ]
            )
        fixture = FIXTURES / "test_contract.c"
        c_binary = temp / "contract-c"
        cxx_binary = temp / "contract-cxx"
        run([cc, "-std=c11", *warnings, str(fixture), "-o", str(c_binary)])
        run([str(c_binary)])
        run(
            [
                cxx,
                "-std=c++20",
                "-x",
                "c++",
                *warnings,
                str(fixture),
                "-o",
                str(cxx_binary),
            ]
        )
        run([str(cxx_binary)])


def check() -> None:
    validate_schema_documents()
    validate_headers()
    validate_profile()
    validate_fixtures()
    validate_w03_goldens()
    compile_contract()
    run(["python3", "scripts/native/semantics/validate-w01.py", "--check"])
    run(["python3", "scripts/native/mir/validate-w02.py", "--check"])
    run(["python3", "scripts/native/lowering/validate-w03.py", "--check"])
    run(["python3", "scripts/native/lowering/test-w03.py", "--self-test"])
    run(["python3", str(GENERATOR.relative_to(ROOT)), "--check"])
    run(["python3", str(OWNERSHIP_CHECKER.relative_to(ROOT)), "--check-manifest"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if not args.check:
        parser.error("only --check is supported")
    try:
        check()
    except (ContractError, OSError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"W04 control-flow contract check failed: {error}", file=sys.stderr)
        return 1
    print(
        "W04 control-flow contract check passed: C11/C++20, W01-W03, "
        "profile, ownership, fixtures, and goldens"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
