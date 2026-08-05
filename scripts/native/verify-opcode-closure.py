#!/usr/bin/env python3
"""Build a fail-closed proof of the current native opcode product closure.

The proof is derived from the live Zend opcode header and the production
lowering, TPDE, compiler, and bounded-runtime sources.  Historical wave
profiles are deliberately not inputs.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


SCHEMA_VERSION = "1.0.0"
EXPECTED_ACTIVE_COUNT = 212
EXPECTED_SENTINEL = 213
EXPECTED_HOLES = [45, 79]

HEADER = "Zend/zend_vm_opcodes.h"
MIR_OPCODES = "Zend/Native/MIR/zend_mir_opcodes.h"
VALUE_LOWERING = "Zend/Native/Values/Lowering/zend_mir_value_lowering.c"
BACKEND = "Zend/Native/TPDE/Common/zend_tpde_backend.cpp"
INTERNAL = "Zend/Native/TPDE/Common/zend_tpde_internal.hpp"
COMPILER = "Zend/Native/Compiler/zend_native_compiler.c"
RUNTIME_CALLS = "Zend/Native/Runtime/Common/zend_native_calls.c"
RUNTIME_REGISTRY = "Zend/Native/Runtime/Common/zend_native_runtime.c"
DARWIN_TARGET = "Zend/Native/TPDE/DarwinA64/zend_tpde_darwin_arm64.cpp"
LINUX_TARGET = "Zend/Native/TPDE/LinuxX64/zend_tpde_linux_x64.cpp"

INPUT_PATHS = (
    HEADER,
    MIR_OPCODES,
    VALUE_LOWERING,
    BACKEND,
    INTERNAL,
    COMPILER,
    RUNTIME_CALLS,
    RUNTIME_REGISTRY,
    DARWIN_TARGET,
    LINUX_TARGET,
)

SPECIAL_PATHS = {
    "ZEND_OP_DATA": {
        "classification": "atomic_owner_sequence",
        "implementation": "executable_tpde",
        "description": (
            "OP_DATA is consumed atomically as the auxiliary operand of its owner "
            "opcode and is marked non-landable in the frozen TPDE source CFG."
        ),
        "anchors": {
            VALUE_LOWERING: (
                "op_array->opcodes[index + 1].opcode == ZEND_OP_DATA",
                "operation->auxiliary = data_opcode.op1;",
            ),
            BACKEND: (
                "op_array->opcodes[source_position + 1].opcode == ZEND_OP_DATA",
                "plan->source_opcode_is_data[source] =",
            ),
        },
    },
    "ZEND_HANDLE_EXCEPTION": {
        "classification": "native_exception_route",
        "implementation": "executable_tpde",
        "description": (
            "Exception dispatch is represented as compiler-built MIR exception "
            "edges and TPDE exception-block branches; no VM opcode handler is called."
        ),
        "anchors": {
            COMPILER: (
                "zend_native_compiler_prepare_exception_routes",
                "zend_mir_zend_op_array_exception_handler",
                "ZEND_NATIVE_SOURCE_EFFECT_EXCEPTION_ROUTE",
            ),
            BACKEND: (
                "candidate.exception_block_id = effect.target_block_id;",
                "instruction.exception_block_id",
            ),
            DARWIN_TARGET: ("instruction->exception_block_id",),
            LINUX_TARGET: ("instruction->exception_block_id",),
        },
    },
    "ZEND_USER_OPCODE": {
        "classification": "native_user_opcode_callback",
        "implementation": "bounded_native_runtime",
        "description": (
            "The native entry invokes the registered user callback, interprets its "
            "action, and resolves ENTER through the native entry registry."
        ),
        "anchors": {
            BACKEND: (
                "ZEND_NATIVE_HELPER_USER_OPCODE_INVOKE",
                "user_opcode_target(",
            ),
            RUNTIME_CALLS: (
                "zend_native_user_opcode_invoke(",
                "zend_get_user_opcode_handler",
                "action = handler(execute_data);",
                "zend_native_execute_observed_frame",
            ),
            DARWIN_TARGET: ("ZEND_USER_OPCODE_DISPATCH",),
            LINUX_TARGET: ("ZEND_USER_OPCODE_DISPATCH",),
        },
    },
    "ZEND_CALL_TRAMPOLINE": {
        "classification": "native_call_trampoline_normalization",
        "implementation": "bounded_native_runtime",
        "description": (
            "Dynamic magic-call trampolines are normalized to __call/__callStatic "
            "and continue through native user/internal call entries."
        ),
        "anchors": {
            BACKEND: ("ZEND_NATIVE_HELPER_USER_CALL_NORMALIZE_RESOLUTION",),
            RUNTIME_CALLS: (
                "zend_native_call_normalize_trampoline(",
                "ZEND_NATIVE_USER_CALL_TARGET_TRAMPOLINE",
                "zend_native_call_normalize_user_resolution(",
                "resolution->ownership &= ~ZEND_NATIVE_USER_CALL_OWNS_TRAMPOLINE;",
            ),
            RUNTIME_REGISTRY: (
                "ZEND_NATIVE_HELPER_USER_CALL_NORMALIZE_RESOLUTION",
                "zend_native_call_normalize_user_resolution",
            ),
        },
    },
}

FORBIDDEN_VM_INTERFACES = {
    "vm_handler_calls": re.compile(r"\bzend_vm_call_opcode_handler\s*\("),
    "execute_ex_fallbacks": re.compile(r"\bexecute_ex\s*\("),
    "opline_handler_calls": re.compile(r"\bopline\s*->\s*handler\b"),
}


class ClosureError(RuntimeError):
    """The live product sources do not prove opcode closure."""


def read_text(root: Path, relative: str) -> str:
    path = root / relative
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        raise ClosureError(f"cannot read {relative}: {exc}") from exc


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def line_of(text: str, anchor: str) -> int:
    offset = text.find(anchor)
    if offset < 0:
        raise ClosureError(f"required product anchor is absent: {anchor}")
    return text.count("\n", 0, offset) + 1


def function_body(text: str, signature: str) -> str:
    search_from = 0
    opening = -1
    while True:
        start = text.find(signature, search_from)
        if start < 0:
            raise ClosureError(f"required product function has no body: {signature}")
        candidate = text.find("{", start)
        semicolon = text.find(";", start)
        if candidate >= 0 and (semicolon < 0 or candidate < semicolon):
            opening = candidate
            break
        search_from = start + len(signature)
    depth = 0
    for index in range(opening, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[opening + 1 : index]
    raise ClosureError(f"function has an unterminated body: {signature}")


def parse_opcode_header(text: str) -> tuple[list[dict[str, Any]], int, list[int]]:
    define = re.compile(r"^#define\s+(ZEND_[A-Z0-9_]+)\s+([0-9]+)\s*$", re.M)
    first = text.find("#define ZEND_NOP")
    last = text.find("#define ZEND_VM_LAST_OPCODE")
    if first < 0 or last < 0 or last <= first:
        raise ClosureError("opcode header lacks its canonical range")
    rows = [
        {"opcode": match.group(1), "number": int(match.group(2))}
        for match in define.finditer(text[first:last])
    ]
    sentinel_match = define.search(text, last)
    if sentinel_match is None or sentinel_match.group(1) != "ZEND_VM_LAST_OPCODE":
        raise ClosureError("opcode sentinel is malformed")
    sentinel = int(sentinel_match.group(2))
    names = [row["opcode"] for row in rows]
    numbers = [row["number"] for row in rows]
    if len(names) != len(set(names)) or len(numbers) != len(set(numbers)):
        raise ClosureError("opcode header contains duplicates")
    holes = sorted(set(range(sentinel + 1)) - set(numbers))
    if len(rows) != EXPECTED_ACTIVE_COUNT:
        raise ClosureError(
            f"expected {EXPECTED_ACTIVE_COUNT} active opcodes, found {len(rows)}"
        )
    if sentinel != EXPECTED_SENTINEL or holes != EXPECTED_HOLES:
        raise ClosureError(
            f"unexpected opcode range: sentinel={sentinel}, holes={holes}"
        )
    return rows, sentinel, holes


def parse_case_blocks(body: str) -> list[tuple[list[str], str]]:
    """Return consecutive case labels and the text through their next return."""
    blocks: list[tuple[list[str], str]] = []
    pending: list[str] = []
    block_start = 0
    case_re = re.compile(r"\bcase\s+(ZEND_[A-Z0-9_]+)\s*:")
    token_re = re.compile(r"\bcase\s+ZEND_[A-Z0-9_]+\s*:|\bdefault\s*:|\breturn\b")
    position = 0
    while True:
        token = token_re.search(body, position)
        if token is None:
            break
        value = token.group(0)
        if value.startswith("case"):
            case = case_re.fullmatch(value)
            assert case is not None
            if not pending:
                block_start = token.start()
            pending.append(case.group(1))
            position = token.end()
            continue
        if value.startswith("default"):
            pending = []
            position = token.end()
            continue
        semicolon = body.find(";", token.end())
        if semicolon < 0:
            raise ClosureError("unterminated return in product switch")
        if pending:
            blocks.append((pending, body[block_start : semicolon + 1]))
            pending = []
        position = semicolon + 1
    return blocks


def parse_return_mapping(body: str) -> dict[str, str]:
    mapping: dict[str, str] = {}
    for cases, block in parse_case_blocks(body):
        match = re.search(r"\breturn\s+(ZEND_[A-Z0-9_]+)\s*;", block)
        if match is None:
            continue
        for case in cases:
            if case in mapping:
                raise ClosureError(f"duplicate case mapping for {case}")
            mapping[case] = match.group(1)
    return mapping


def parse_direct_targets(body: str) -> dict[str, dict[str, str | None]]:
    targets: dict[str, dict[str, str | None]] = {}
    for cases, block in parse_case_blocks(body):
        if re.search(r"\breturn\s+true\s*;", block) is None:
            continue
        kind = re.search(
            r"target->kind\s*=\s*(ZEND_TPDE_USER_OPCODE_TARGET_[A-Z0-9_]+)",
            block,
        )
        if kind is None:
            raise ClosureError(f"direct target lacks a kind: {', '.join(cases)}")
        helper = re.search(
            r"target->helper\s*=\s*(ZEND_NATIVE_HELPER_[A-Z0-9_]+)", block
        )
        for case in cases:
            targets[case] = {
                "target_kind": kind.group(1),
                "helper": helper.group(1) if helper is not None else None,
            }
    return targets


def parse_mir_values(text: str) -> dict[str, int]:
    values: dict[str, int] = {}
    catalog = re.compile(r'\bX\(([A-Z0-9_]+),\s*"[^"]+",\s*([0-9]+)\)')
    for match in catalog.finditer(text):
        values.setdefault("ZEND_MIR_OPCODE_" + match.group(1), int(match.group(2)))
    constants = re.compile(r"\b(ZEND_MIR_[A-Z0-9_]+)\s*=\s*([0-9]+)")
    for match in constants.finditer(text):
        values.setdefault(match.group(1), int(match.group(2)))
    if not values:
        raise ClosureError("MIR opcode catalog is absent")
    return values


def parse_ranges(body: str) -> list[tuple[str, str]]:
    return re.findall(
        r"opcode\s*>=\s*(ZEND_MIR_OPCODE_[A-Z0-9_]+)\s*"
        r"&&\s*opcode\s*<=\s*(ZEND_MIR_OPCODE_[A-Z0-9_]+)",
        body,
    )


def parse_exclusive_ranges(body: str) -> list[tuple[str, str]]:
    return re.findall(
        r"opcode\s*>=\s*(ZEND_MIR_OPCODE_[A-Z0-9_]+)\s*"
        r"&&\s*opcode\s*<\s*(ZEND_MIR_[A-Z0-9_]+)",
        body,
    )


def in_ranges(
    symbol: str, ranges: list[tuple[str, str]], values: dict[str, int]
) -> bool:
    if symbol not in values:
        return False
    value = values[symbol]
    for first, last in ranges:
        if first not in values or last not in values:
            raise ClosureError(f"MIR range uses an unknown symbol: {first}..{last}")
        if values[first] <= value <= values[last]:
            return True
    return False


def in_exclusive_ranges(
    symbol: str, ranges: list[tuple[str, str]], values: dict[str, int]
) -> bool:
    if symbol not in values:
        return False
    value = values[symbol]
    for first, limit in ranges:
        if first not in values or limit not in values:
            raise ClosureError(f"MIR range uses an unknown symbol: {first}..{limit}")
        if values[first] <= value < values[limit]:
            return True
    return False


def source_ref(relative: str, text: str, anchor: str) -> dict[str, Any]:
    return {"path": relative, "line": line_of(text, anchor), "anchor": anchor}


def validate_special_paths(
    texts: dict[str, str]
) -> dict[str, list[dict[str, Any]]]:
    refs: dict[str, list[dict[str, Any]]] = {}
    for opcode, contract in SPECIAL_PATHS.items():
        opcode_refs: list[dict[str, Any]] = []
        for relative, anchors in contract["anchors"].items():
            source = texts[relative]
            for anchor in anchors:
                opcode_refs.append(source_ref(relative, source, anchor))
        refs[opcode] = opcode_refs
    return refs


def scan_forbidden_vm_interfaces(root: Path) -> tuple[dict[str, int], int, str]:
    native_root = root / "Zend/Native"
    suffixes = {".c", ".cc", ".cpp", ".h", ".hpp", ".inc"}
    paths = sorted(
        path for path in native_root.rglob("*") if path.is_file() and path.suffix in suffixes
    )
    if not paths:
        raise ClosureError("native product source tree is empty")
    counts = {name: 0 for name in FORBIDDEN_VM_INTERFACES}
    digest = hashlib.sha256()
    for path in paths:
        relative = path.relative_to(root).as_posix()
        data = path.read_bytes()
        digest.update(relative.encode("utf-8") + b"\0" + data + b"\0")
        text = data.decode("utf-8", errors="replace")
        for name, pattern in FORBIDDEN_VM_INTERFACES.items():
            counts[name] += len(pattern.findall(text))
    nonzero = {name: count for name, count in counts.items() if count != 0}
    if nonzero:
        raise ClosureError(f"forbidden VM acceptance is present: {nonzero}")
    return counts, len(paths), digest.hexdigest()


def build_evidence(root: Path) -> dict[str, Any]:
    root = root.resolve()
    texts = {relative: read_text(root, relative) for relative in INPUT_PATHS}
    rows, sentinel, holes = parse_opcode_header(texts[HEADER])

    direct_body = function_body(texts[BACKEND], "bool user_opcode_target(")
    direct = parse_direct_targets(direct_body)
    w12_body = function_body(
        texts[VALUE_LOWERING], "zend_mir_opcode zend_mir_w12_executable_opcode("
    )
    w12 = parse_return_mapping(w12_body)
    helper_body = function_body(
        texts[BACKEND], "zend_native_runtime_helper_id executable_value_helper("
    )
    helper_map = parse_return_mapping(helper_body)
    helper_ranges = parse_ranges(helper_body)
    executable_body = function_body(
        texts[MIR_OPCODES], "static inline bool zend_mir_opcode_is_executable_value("
    )
    executable_ranges = parse_ranges(executable_body)
    executable_exclusive_ranges = parse_exclusive_ranges(executable_body)
    executable_singles = set(
        re.findall(r"opcode\s*==\s*(ZEND_MIR_OPCODE_[A-Z0-9_]+)", executable_body)
    )
    mir_values = parse_mir_values(texts[MIR_OPCODES])
    special_refs = validate_special_paths(texts)

    opcode_records: list[dict[str, Any]] = []
    unclassified: list[str] = []
    for row in rows:
        opcode = row["opcode"]
        record: dict[str, Any] = {"number": row["number"], "opcode": opcode}
        if opcode in SPECIAL_PATHS:
            contract = SPECIAL_PATHS[opcode]
            record.update(
                classification=contract["classification"],
                implementation=contract["implementation"],
                description=contract["description"],
                source_refs=special_refs[opcode],
            )
        elif opcode in direct:
            target = direct[opcode]
            record.update(
                classification="direct_tpde_target",
                implementation="executable_tpde",
                target_kind=target["target_kind"],
                helper=target["helper"],
                source_refs=[source_ref(BACKEND, texts[BACKEND], f"case {opcode}:")],
            )
            for target_path in (DARWIN_TARGET, LINUX_TARGET):
                if target["target_kind"] not in texts[target_path]:
                    raise ClosureError(
                        f"{target_path} does not emit {target['target_kind']} for {opcode}"
                    )
        elif opcode in w12:
            mir = w12[opcode]
            executable = (
                mir in executable_singles
                or in_ranges(mir, executable_ranges, mir_values)
                or in_exclusive_ranges(mir, executable_exclusive_ranges, mir_values)
            )
            helper = helper_map.get(mir)
            derived_helper = in_ranges(mir, helper_ranges, mir_values)
            if not executable or (helper is None and not derived_helper):
                raise ClosureError(
                    f"{opcode} maps to {mir} without an executable bounded helper"
                )
            helper_anchor = mir
            if helper is None:
                helper_anchor = next(
                    first
                    for first, last in helper_ranges
                    if in_ranges(mir, [(first, last)], mir_values)
                )
            record.update(
                classification="bounded_runtime_helper",
                implementation="bounded_native_runtime",
                mir_opcode=mir,
                helper=helper if helper is not None else "derived_contiguous_helper",
                source_refs=[
                    source_ref(VALUE_LOWERING, texts[VALUE_LOWERING], f"case {opcode}:"),
                    source_ref(BACKEND, texts[BACKEND], helper_anchor),
                ],
            )
        else:
            unclassified.append(opcode)
            continue
        opcode_records.append(record)

    if unclassified:
        raise ClosureError(
            "active opcodes without a current product path: " + ", ".join(unclassified)
        )
    if len(opcode_records) != EXPECTED_ACTIVE_COUNT:
        raise ClosureError("product closure is not exactly 212/212")

    vm_counts, scanned_file_count, native_tree_sha256 = scan_forbidden_vm_interfaces(root)
    model_only = sum(record["implementation"] == "model_only" for record in opcode_records)
    deferred = sum(record["implementation"] == "deferred" for record in opcode_records)
    unsupported_valid = EXPECTED_ACTIVE_COUNT - len(opcode_records)
    metrics = {
        "active_opcodes_without_product_path": unsupported_valid,
        "model_only_paths": model_only,
        "deferred_paths": deferred,
        "unsupported_valid_paths": unsupported_valid,
        **vm_counts,
    }
    if any(metrics.values()):
        raise ClosureError(f"hard-zero closure metrics are nonzero: {metrics}")

    counts: dict[str, int] = {}
    for record in opcode_records:
        key = record["classification"]
        counts[key] = counts.get(key, 0) + 1

    source_hashes = {
        relative: sha256_bytes((root / relative).read_bytes()) for relative in INPUT_PATHS
    }
    return {
        "schema_version": SCHEMA_VERSION,
        "proof_kind": "current_native_product_opcode_closure",
        "historical_wave_artifacts_used": False,
        "source_basis": {
            "files": source_hashes,
            "native_product_source_file_count": scanned_file_count,
            "native_product_tree_sha256": native_tree_sha256,
        },
        "opcode_set": {
            "active_count": len(rows),
            "sentinel": sentinel,
            "holes": holes,
        },
        "summary": {
            "covered_count": len(opcode_records),
            "classification_counts": dict(sorted(counts.items())),
            "hard_zero_metrics": metrics,
        },
        "opcodes": opcode_records,
    }


def serialized(evidence: dict[str, Any]) -> str:
    return json.dumps(evidence, indent=2, sort_keys=True) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    default_root = Path(__file__).resolve().parents[2]
    parser.add_argument("--root", type=Path, default=default_root)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("docs/native-engine/semantics/opcodes/product-opcode-closure.json"),
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true", help="write current evidence")
    mode.add_argument("--check", action="store_true", help="verify checked-in evidence")
    args = parser.parse_args(argv)
    try:
        evidence = build_evidence(args.root)
        rendered = serialized(evidence)
        output = args.output if args.output.is_absolute() else args.root / args.output
        if args.write:
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(rendered, encoding="utf-8")
        elif args.check:
            try:
                existing = output.read_text(encoding="utf-8")
            except OSError as exc:
                raise ClosureError(f"cannot read proof artifact {output}: {exc}") from exc
            if existing != rendered:
                raise ClosureError(
                    "product opcode closure artifact is stale; run verifier with --write"
                )
        else:
            sys.stdout.write(rendered)
            return 0
        print(
            "current native opcode closure: "
            f"{evidence['summary']['covered_count']}/{EXPECTED_ACTIVE_COUNT}, "
            "all hard-zero metrics satisfied"
        )
        return 0
    except ClosureError as exc:
        print(f"opcode closure failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
