#!/usr/bin/env python3
"""Validate the frozen, target-neutral ZNMIR C contract."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[3]
MIR = ROOT / "Zend" / "Native" / "MIR"
EFFECT_MODEL = ROOT / "docs" / "native-engine" / "semantics" / "effects" / "effect-model.json"
FRAME_SCHEMA = ROOT / "docs" / "native-engine" / "semantics" / "frames" / "frame-state.schema.json"
FIXTURES = ROOT / "tests" / "native" / "mir" / "contracts"

HEADERS = (
    "zend_mir.h",
    "zend_mir_ids.h",
    "zend_mir_opcodes.h",
    "zend_mir_effects.h",
    "zend_mir_frame_state.h",
    "zend_mir_diagnostic.h",
    "zend_mir_private.h",
)

EFFECT_CATALOGS = {
    "ZEND_MIR_EFFECT_CATALOG": "effects",
    "ZEND_MIR_MEMORY_DOMAIN_CATALOG": "memory_domains",
    "ZEND_MIR_OWNERSHIP_STATE_CATALOG": "ownership_states",
    "ZEND_MIR_OWNERSHIP_ACTION_CATALOG": "ownership_actions",
    "ZEND_MIR_BARRIER_CATALOG": "barriers",
    "ZEND_MIR_PREDICATE_CATALOG": "predicates",
    "ZEND_MIR_GUARD_FACT_CATALOG": "guard_facts",
    "ZEND_MIR_COMPOSITION_RULE_CATALOG": "composition_rules",
}

FRAME_CATALOG_PATHS = {
    "ZEND_MIR_FUNCTION_KIND_CATALOG": ("properties", "function", "properties", "kind", "enum"),
    "ZEND_MIR_OPLINE_PHASE_CATALOG": ("properties", "opline", "properties", "phase", "enum"),
    "ZEND_MIR_SUSPEND_KIND_CATALOG": ("properties", "suspend", "properties", "kind", "enum"),
    "ZEND_MIR_RESUME_ENTRY_KIND_CATALOG": ("properties", "resume", "properties", "entry_kind", "enum"),
    "ZEND_MIR_SAFEPOINT_CLASS_CATALOG": ("properties", "safepoint", "properties", "class", "enum"),
    "ZEND_MIR_FRAME_SLOT_KIND_CATALOG": ("$defs", "slot", "properties", "kind", "enum"),
    "ZEND_MIR_FRAME_SLOT_REPRESENTATION_CATALOG": ("$defs", "slot", "properties", "representation", "enum"),
    "ZEND_MIR_MATERIALIZATION_CATALOG": ("$defs", "slot", "properties", "materialization", "enum"),
    "ZEND_MIR_FRAME_SLOT_OWNERSHIP_CATALOG": ("$defs", "slot", "properties", "ownership", "enum"),
    "ZEND_MIR_CLEANUP_ACTION_CATALOG": ("$defs", "cleanup", "properties", "action", "enum"),
    "ZEND_MIR_CLEANUP_STATE_CATALOG": ("$defs", "cleanup", "properties", "state", "enum"),
    "ZEND_MIR_CONTINUATION_KIND_CATALOG": ("$defs", "continuation", "properties", "kind", "enum"),
}

CORE_CATALOGS = {
    "ZEND_MIR_OPCODE_CATALOG": [
        "constant", "phi", "copy", "canonicalize", "statepoint", "branch",
        "cond_branch", "return", "throw", "unreachable",
    ],
    "ZEND_MIR_REPRESENTATION_CATALOG": [
        "void", "control", "i1", "i8", "i16", "i32", "i64", "double",
        "semantic_pointer", "zval",
    ],
    "ZEND_MIR_CONSTANT_KIND_CATALOG": [
        "signed_integer_bits", "double_bits", "null", "false", "true",
        "string_symbol", "semantic_pointer_symbol",
    ],
}

INVALID_ENUM_CONSTANTS = (
    "ZEND_MIR_OPCODE_INVALID",
    "ZEND_MIR_REPRESENTATION_INVALID",
    "ZEND_MIR_EFFECT_INVALID",
    "ZEND_MIR_MEMORY_DOMAIN_INVALID",
    "ZEND_MIR_OWNERSHIP_STATE_INVALID",
    "ZEND_MIR_OWNERSHIP_ACTION_INVALID",
    "ZEND_MIR_BARRIER_INVALID",
    "ZEND_MIR_PREDICATE_INVALID",
    "ZEND_MIR_GUARD_FACT_INVALID",
    "ZEND_MIR_COMPOSITION_RULE_INVALID",
    "ZEND_MIR_CONSTANT_KIND_INVALID",
    "ZEND_MIR_FUNCTION_KIND_INVALID",
    "ZEND_MIR_OPLINE_PHASE_INVALID",
    "ZEND_MIR_SUSPEND_KIND_INVALID",
    "ZEND_MIR_RESUME_ENTRY_KIND_INVALID",
    "ZEND_MIR_SAFEPOINT_CLASS_INVALID",
    "ZEND_MIR_FRAME_SLOT_KIND_INVALID",
    "ZEND_MIR_FRAME_SLOT_REPRESENTATION_INVALID",
    "ZEND_MIR_MATERIALIZATION_INVALID",
    "ZEND_MIR_FRAME_SLOT_OWNERSHIP_INVALID",
    "ZEND_MIR_CLEANUP_ACTION_INVALID",
    "ZEND_MIR_CLEANUP_STATE_INVALID",
    "ZEND_MIR_CONTINUATION_KIND_INVALID",
    "ZEND_MIR_DIAGNOSTIC_SEVERITY_INVALID",
    "ZEND_MIR_DIAGNOSTIC_CODE_INVALID",
)

SERIALIZABLE_STRUCTS = (
    "zend_mir_span",
    "zend_mir_source_position_ref",
    "zend_mir_source_map_ref",
    "zend_mir_frame_slot_ref",
    "zend_mir_cleanup_ref",
    "zend_mir_continuation_ref",
    "zend_mir_resume_ref",
    "zend_mir_frame_state_ref",
    "zend_mir_diagnostic_location",
    "zend_mir_diagnostic",
    "zend_mir_function_record",
    "zend_mir_block_record",
    "zend_mir_value_record",
    "zend_mir_constant_record",
    "zend_mir_instruction_record",
)

FORBIDDEN_INCLUDE = re.compile(
    r"^\s*#\s*include\s*[<\"][^>\"]*(?:tpde|x86|aarch64|arm64|riscv|zend_jit|zend_vm)[^>\"]*[>\"]",
    re.IGNORECASE | re.MULTILINE,
)
CATALOG_ENTRY = re.compile(r'^\s*X\([^,]+,\s*"([^"]+)",\s*([0-9]+)\)')


class ContractError(RuntimeError):
    pass


def load_json(path: Path) -> object:
    with path.open("r", encoding="utf-8") as source:
        return json.load(source)


def nested(data: object, path: tuple[str, ...]) -> object:
    current = data
    for key in path:
        if not isinstance(current, dict) or key not in current:
            raise ContractError(f"missing W01 schema path: {'/'.join(path)}")
        current = current[key]
    return current


def parse_catalog(text: str, macro: str) -> list[tuple[str, int]]:
    lines = text.splitlines()
    marker = f"#define {macro}(X)"
    for start, line in enumerate(lines):
        if line.startswith(marker):
            entries: list[tuple[str, int]] = []
            for catalog_line in lines[start + 1 :]:
                match = CATALOG_ENTRY.match(catalog_line)
                if match is None:
                    break
                entries.append((match.group(1), int(match.group(2))))
                if not catalog_line.rstrip().endswith("\\"):
                    break
            if not entries:
                raise ContractError(f"{macro} is empty or malformed")
            return entries
    raise ContractError(f"missing catalog {macro}")


def validate_catalog_text(text: str, macro: str, expected: list[str]) -> None:
    entries = parse_catalog(text, macro)
    labels = [label for label, _ in entries]
    values = [value for _, value in entries]
    if labels != expected:
        raise ContractError(f"{macro} does not exactly match W01 order: {labels!r}")
    if values != list(range(len(expected))):
        raise ContractError(f"{macro} IDs must be unique and contiguous from zero: {values!r}")
    if len(values) != len(set(values)):
        raise ContractError(f"{macro} contains duplicate IDs")


def extract_struct(text: str, name: str) -> str:
    pattern = re.compile(
        rf"typedef\s+struct\s+_{re.escape(name)}\s*\{{(?P<body>.*?)\}}\s*{re.escape(name)}\s*;",
        re.DOTALL,
    )
    match = pattern.search(text)
    if match is None:
        raise ContractError(f"missing serializable structure {name}")
    return match.group("body")


def validate_serializable_text(text: str, names: tuple[str, ...]) -> None:
    for name in names:
        body = extract_struct(text, name)
        if "*" in body:
            raise ContractError(f"raw pointer field in serializable structure {name}")


def validate_includes_text(text: str, label: str) -> None:
    match = FORBIDDEN_INCLUDE.search(text)
    if match is not None:
        raise ContractError(f"forbidden target/TPDE include in {label}: {match.group(0).strip()}")


def validate_target_neutral_fields(text: str) -> None:
    forbidden = re.search(
        r"\b(?:x86|x64|amd64|aarch64|arm64|riscv|tpde)(?:_[A-Za-z0-9]+)*\b"
        r"|\b(?:physical|machine|target)_(?:register|reg|opcode|offset|address|feature)(?:_[A-Za-z0-9]+)*\b"
        r"|\b(?:register|reg_class|regclass|vm_dispatch)\b",
        text,
        re.IGNORECASE,
    )
    if forbidden is not None:
        raise ContractError(f"target-dependent public field or type: {forbidden.group(0)}")


def validate_ids_text(text: str) -> None:
    required = {
        "ZEND_MIR_ID_INVALID": "UINT32_C(0xffffffff)",
        "ZEND_MIR_ID_MAX": "UINT32_C(0xfffffffe)",
        "ZEND_MIR_VALUE_SYNTHETIC_BIT": "UINT32_C(0x80000000)",
        "ZEND_MIR_VALUE_ORIGINAL_MAX": "UINT32_C(0x7fffffff)",
        "ZEND_MIR_VALUE_SYNTHETIC_MAX": "UINT32_C(0xfffffffe)",
    }
    for name, value in required.items():
        if not re.search(rf"^#define {name} {re.escape(value)}$", text, re.MULTILINE):
            raise ContractError(f"missing or invalid ID constant {name}")
    invalid_definitions = re.findall(r"^#define\s+ZEND_MIR_ID_INVALID\b", text, re.MULTILINE)
    if len(invalid_definitions) != 1:
        raise ContractError("the ID namespace must have exactly one invalid sentinel definition")


def validate_explicit_enum(text: str, enum_name: str) -> None:
    match = re.search(
        rf"typedef\s+enum\s+_{re.escape(enum_name)}\s*\{{(?P<body>.*?)\}}\s*{re.escape(enum_name)}\s*;",
        text,
        re.DOTALL,
    )
    if match is None:
        raise ContractError(f"missing explicit enum {enum_name}")
    values: list[int] = []
    for line in match.group("body").splitlines():
        item = re.match(r"\s*[A-Z][A-Z0-9_]*\s*=\s*([0-9]+)\s*,?\s*$", line)
        if item is not None:
            values.append(int(item.group(1)))
    if not values or len(values) != len(set(values)):
        raise ContractError(f"{enum_name} contains missing or duplicate explicit IDs")


def validate_enum_sentinels(text: str) -> None:
    for name in INVALID_ENUM_CONSTANTS:
        definitions = re.findall(rf"^\s*{name}\s*=\s*UINT32_MAX\s*,?\s*$", text, re.MULTILINE)
        if len(definitions) != 1:
            raise ContractError(f"invalid or duplicate enum sentinel {name}")


def validate_sources() -> None:
    sources = {name: (MIR / name).read_text(encoding="utf-8") for name in HEADERS}
    combined = "\n".join(sources.values())
    effects = load_json(EFFECT_MODEL)
    frame = load_json(FRAME_SCHEMA)
    if not isinstance(effects, dict) or not isinstance(effects.get("catalog"), dict):
        raise ContractError("invalid W01 effect model")

    effect_header = sources["zend_mir_effects.h"]
    for macro, key in EFFECT_CATALOGS.items():
        expected = effects["catalog"].get(key)
        if not isinstance(expected, list) or not all(isinstance(item, str) for item in expected):
            raise ContractError(f"invalid W01 catalog {key}")
        validate_catalog_text(effect_header, macro, expected)

    frame_header = sources["zend_mir_frame_state.h"]
    for macro, path in FRAME_CATALOG_PATHS.items():
        expected = nested(frame, path)
        if not isinstance(expected, list) or not all(isinstance(item, str) for item in expected):
            raise ContractError(f"invalid W01 frame catalog {'/'.join(path)}")
        validate_catalog_text(frame_header, macro, expected)

    opcode_header = sources["zend_mir_opcodes.h"]
    for macro, expected in CORE_CATALOGS.items():
        validate_catalog_text(opcode_header, macro, expected)

    validate_ids_text(sources["zend_mir_ids.h"])
    validate_explicit_enum(sources["zend_mir_diagnostic.h"], "zend_mir_diagnostic_code")
    validate_explicit_enum(sources["zend_mir_diagnostic.h"], "zend_mir_diagnostic_severity")
    validate_enum_sentinels(combined)
    validate_serializable_text(combined, SERIALIZABLE_STRUCTS)
    validate_target_neutral_fields(combined)
    for name, source in sources.items():
        validate_includes_text(source, name)

    fixture_sources = {
        name: (FIXTURES / name).read_text(encoding="utf-8")
        for name in ("fixture_host.h", "fixture_host.c", "fixture_smoke.c")
    }
    for name, fixture_source in fixture_sources.items():
        validate_includes_text(fixture_source, name)
    fixture_source = fixture_sources["fixture_host.c"]
    if re.search(r"\bswitch\s*\([^)]*opcode", fixture_source):
        raise ContractError("fixture host must not contain an opcode dispatcher")


def run(command: list[str], *, cwd: Path = ROOT) -> None:
    completed = subprocess.run(command, cwd=cwd, text=True, capture_output=True, check=False)
    if completed.returncode != 0:
        output = "\n".join(part for part in (completed.stdout, completed.stderr) if part)
        raise ContractError(f"command failed ({' '.join(command)}):\n{output.rstrip()}")


def compiler(name: str, env_name: str) -> str:
    requested = os.environ.get(env_name, name)
    resolved = shutil.which(requested)
    if resolved is None:
        raise ContractError(f"required compiler not found: {requested}")
    return resolved


def compile_contract() -> None:
    cc = compiler("cc", "CC")
    cxx = compiler("c++", "CXX")
    with tempfile.TemporaryDirectory(prefix="znmir-contract-") as temporary:
        temp = Path(temporary)
        for header in HEADERS:
            stem = header.replace(".", "_")
            c_source = temp / f"{stem}.c"
            cxx_source = temp / f"{stem}.cc"
            include = f'#include "Zend/Native/MIR/{header}"\nint main(void) {{ return 0; }}\n'
            c_source.write_text(include, encoding="utf-8")
            cxx_source.write_text(include, encoding="utf-8")
            run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT), "-c",
                 str(c_source), "-o", str(temp / f"{stem}.o")])
            run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT), "-c",
                 str(cxx_source), "-o", str(temp / f"{stem}.opp")])

        c_fixture = temp / "fixture-c"
        cxx_fixture = temp / "fixture-cxx"
        fixture_args = [
            "-Wall", "-Wextra", "-Werror", "-I", str(ROOT), "-I", str(FIXTURES),
            str(FIXTURES / "fixture_host.c"), str(FIXTURES / "fixture_smoke.c"),
        ]
        run([cc, "-std=c11", *fixture_args, "-o", str(c_fixture)])
        run([str(c_fixture)])
        run([cxx, "-std=c++20", "-x", "c++", *fixture_args, "-o", str(cxx_fixture)])
        run([str(cxx_fixture)])


def check() -> None:
    validate_sources()
    compile_contract()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="validate without modifying files")
    arguments = parser.parse_args()
    if not arguments.check:
        parser.error("only read-only --check mode is supported")
    try:
        check()
    except (ContractError, OSError, json.JSONDecodeError) as error:
        print(f"ZNMIR contract check failed: {error}", file=sys.stderr)
        return 1
    print("ZNMIR contract check passed: W01 catalogs, headers, and fixture host are valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
