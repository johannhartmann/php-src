#!/usr/bin/env python3
"""Validate the frozen W03 lowering contract without modifying the tree."""

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

ROOT = Path(__file__).resolve().parents[3]
LOWERING = ROOT / "Zend/Native/Lowering"
MIR = ROOT / "Zend/Native/MIR"
FIXTURES = ROOT / "tests/native/lowering/contracts"
PROFILE = ROOT / "docs/native-engine/lowering/w03-opcode-profile.json"
MATRIX = ROOT / "docs/native-engine/semantics/opcodes/opcode-matrix.json"
GENERATOR = ROOT / "scripts/native/lowering/generate-profile.py"

HEADERS = (
    MIR / "zend_mir.h",
    MIR / "zend_mir_ids.h",
    MIR / "zend_mir_opcodes.h",
    MIR / "zend_mir_diagnostic.h",
    MIR / "zend_mir_scalar.h",
    LOWERING / "zend_mir_lowering.h",
    LOWERING / "zend_mir_lowering_source.h",
    LOWERING / "zend_mir_lowering_registry.h",
    LOWERING / "zend_mir_lowering_diagnostic.h",
)

RECORDS = (
    "zend_mir_value_fact_ref",
    "zend_mir_source_operand_ref",
    "zend_mir_source_opcode_ref",
    "zend_mir_source_ssa_ref",
    "zend_mir_source_ssa_use_ref",
    "zend_mir_source_ssa_def_ref",
    "zend_mir_source_literal_ref",
    "zend_mir_lowering_claim",
)


class ContractError(RuntimeError):
    pass


def run(command: list[str]) -> None:
    completed = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, check=False)
    if completed.returncode:
        output = "\n".join(part for part in (completed.stdout, completed.stderr) if part)
        raise ContractError(f"command failed ({' '.join(command)}):\n{output.rstrip()}")


def extract_struct(text: str, name: str) -> str:
    match = re.search(
        rf"typedef\s+struct\s+_{re.escape(name)}\s*\{{(?P<body>.*?)\}}\s*{re.escape(name)}\s*;",
        text,
        re.DOTALL,
    )
    if match is None:
        raise ContractError(f"missing immutable record {name}")
    return match.group("body")


def load_generator():
    spec = importlib.util.spec_from_file_location("w03_generate_profile", GENERATOR)
    if spec is None or spec.loader is None:
        raise ContractError("cannot load profile generator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def validate_profile() -> None:
    generator = load_generator()
    matrix = json.loads(MATRIX.read_text(encoding="utf-8"))
    profile = json.loads(PROFILE.read_text(encoding="utf-8"))
    errors = generator.validate_profile(profile, matrix)
    if errors:
        raise ContractError("; ".join(errors))
    generated = generator.build_profile(matrix)
    if profile != generated:
        raise ContractError("generated profile drift; run generate-profile.py --write")
    classifications = {entry["opcode"]: entry["classification"] for entry in profile["opcodes"]}
    for name in ("ZEND_DIV", "ZEND_POW", "ZEND_ASSIGN", "ZEND_JMP", "ZEND_DO_FCALL"):
        if classifications.get(name) != "deferred":
            raise ContractError(f"mandatory exclusion is not deferred: {name}")
    if profile["active_opcode_count"] != 212:
        raise ContractError("profile must follow the live 212-opcode W01 source")


def validate_headers() -> None:
    sources = {path: path.read_text(encoding="utf-8") for path in HEADERS}
    combined = "\n".join(sources.values())
    ids = sources[MIR / "zend_mir_ids.h"]
    if not re.search(r"ZEND_MIR_CONTRACT_VERSION_MINOR UINT32_C\(2\)", ids):
        raise ContractError("MIR contract version is not 1.2")
    opcode_text = sources[MIR / "zend_mir_opcodes.h"]
    expected_core = {
        "CONSTANT": 0, "PHI": 1, "COPY": 2, "CANONICALIZE": 3, "STATEPOINT": 4,
        "BRANCH": 5, "COND_BRANCH": 6, "RETURN": 7, "THROW": 8, "UNREACHABLE": 9,
    }
    for symbol, value in expected_core.items():
        if not re.search(rf'X\({symbol},\s*"[^"]+",\s*{value}\)', opcode_text):
            raise ContractError(f"W02 opcode identity drifted: {symbol}")
    scalar_entries = re.findall(
        r'X\(([A-Z0-9_]+),\s*"([a-z0-9_]+)",\s*([0-9]+)\)',
        opcode_text.split("#define ZEND_MIR_SCALAR_OPCODE_CATALOG", 1)[1].split(
            "#define ZEND_MIR_OPCODE_ENUM", 1
        )[0],
    )
    values = [int(value) for _, _, value in scalar_entries]
    if values != list(range(10, 41)):
        raise ContractError("scalar opcode IDs must be explicit and contiguous 10..40")
    required_tokens = (
        "zend_mir_php_scalar_type_mask",
        "zend_mir_value_fact_ref",
        "value_fact_count",
        "value_fact_at",
        "add_value_fact",
        "zend_mir_verify_w03_scalar",
        "zend_mir_lowering_source_view",
        "zend_mir_lowering_context",
        "zend_mir_lowering_provider",
        "zend_mir_lowering_registry",
        "zend_mir_lowering_status",
        "zend_mir_lowering_code",
        "ZEND_MIR_LOWERING_GUARANTEE_STAGE2_VERIFIED",
    )
    for token in required_tokens:
        if token not in combined:
            raise ContractError(f"missing W03 ABI token {token}")
    for number in range(14):
        token = f"[MIRL{number:04d}]"
        if token not in combined:
            raise ContractError(f"missing stable lowering token {token}")
    for record in RECORDS:
        if "*" in extract_struct(combined, record):
            raise ContractError(f"raw pointer in immutable record {record}")
    lowering_boundary = "\n".join(
        sources[path] for path in HEADERS
        if path.parent == LOWERING or path.name == "zend_mir_scalar.h"
    )
    forbidden = re.search(
        r"#\s*include[^\n]*(?:tpde|x86|aarch64|arm64|riscv|zend_vm|zend_jit)"
        r"|\b(?:zend_op_array|zend_op|zval|zend_ssa)\b",
        lowering_boundary,
        re.IGNORECASE,
    )
    if forbidden:
        raise ContractError(f"forbidden target/runtime type in W03 ABI: {forbidden.group(0)}")
    lowering_header = sources[LOWERING / "zend_mir_lowering.h"]
    if "result->module != NULL" not in lowering_header or "result->module == NULL" not in lowering_header:
        raise ContractError("failure-atomic module invariant is not encoded")


def compiler(default: str, variable: str) -> str:
    selected = os.environ.get(variable, default)
    resolved = shutil.which(selected)
    if resolved is None:
        raise ContractError(f"required compiler not found: {selected}")
    return resolved


def compile_contract() -> None:
    cc = compiler("cc", "CC")
    cxx = compiler("c++", "CXX")
    with tempfile.TemporaryDirectory(prefix="w03-contract-") as temporary:
        temp = Path(temporary)
        for header in HEADERS:
            relative = header.relative_to(ROOT).as_posix()
            stem = header.stem
            body = f'#include "{relative}"\nint main(void) {{ return 0; }}\n'
            c_source = temp / f"{stem}.c"
            cxx_source = temp / f"{stem}.cc"
            c_source.write_text(body, encoding="utf-8")
            cxx_source.write_text(body, encoding="utf-8")
            warnings = ["-pedantic-errors", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT)]
            run([cc, "-std=c11", *warnings, "-c", str(c_source), "-o", str(temp / f"{stem}.o")])
            run([cxx, "-std=c++20", *warnings, "-c", str(cxx_source), "-o", str(temp / f"{stem}.opp")])
        fixture_sources = [
            str(FIXTURES / "lowering_fixture_host.c"),
            str(FIXTURES / "test_contract.c"),
        ]
        warnings = ["-pedantic-errors", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT), "-I", str(FIXTURES)]
        c_binary = temp / "fixture-c"
        cxx_binary = temp / "fixture-cxx"
        run([cc, "-std=c11", *warnings, *fixture_sources, "-o", str(c_binary)])
        run([str(c_binary)])
        run([cxx, "-std=c++20", "-x", "c++", *warnings, *fixture_sources, "-o", str(cxx_binary)])
        run([str(cxx_binary)])


def check() -> None:
    validate_profile()
    validate_headers()
    compile_contract()
    run(["python3", "tests/native/mir/text/run_text_tests.py"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if not args.check:
        parser.error("only --check is supported")
    try:
        check()
    except (ContractError, OSError, json.JSONDecodeError) as error:
        print(f"W03 lowering contract check failed: {error}", file=sys.stderr)
        return 1
    print("W03 lowering contract check passed: profile, ABI, fixtures, and W02 goldens")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
