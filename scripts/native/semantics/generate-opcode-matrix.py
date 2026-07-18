#!/usr/bin/env python3
"""Generate the W01 Zend opcode coverage matrix.

Mechanical opcode and handler structure comes from Zend sources. Semantic
claims come only from the reviewable opcode-overrides.json input.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable


FORMAT_VERSION = "1.0.0"
SOURCE_COMMIT = "b7c524a19fa815799a858b98d39f176ca88648b1"

SOURCE_PATHS = (
    "Zend/zend_vm_opcodes.h",
    "Zend/zend_vm_def.h",
    "Zend/zend_vm_gen.php",
    "Zend/zend_compile.h",
    "Zend/zend_execute.c",
    "Zend/zend_execute.h",
    "Zend/zend_partial.c",
    "Zend/Optimizer/zend_ssa.h",
    "ext/opcache/jit/README.md",
    "ext/opcache/jit/zend_jit.c",
    "ext/opcache/jit/zend_jit_ir.c",
    "ext/opcache/jit/zend_jit_trace.c",
    "ext/opcache/jit/zend_jit_vm_helpers.c",
)

EFFECT_IDS = {
    "read_memory", "write_memory", "allocate", "throw", "bailout",
    "call_internal", "call_php", "reenter_php", "run_destructor",
    "observe_frame", "interrupt_boundary", "suspend", "resume",
    "external_io", "terminate",
}
MEMORY_DOMAINS = {
    "frame.args", "frame.locals", "frame.temps", "frame.call_chain",
    "runtime.symbol_table", "runtime.cache", "heap.zval", "heap.array",
    "heap.object", "heap.string", "heap.reference", "gc.metadata",
    "engine.exception", "engine.observer", "engine.interrupt",
    "engine.class_table", "engine.function_table", "engine.generator",
    "engine.fiber", "external.state",
}
OWNERSHIP_ACTIONS = {
    "borrow", "copy_addref", "move", "produce_owned", "produce_borrowed",
    "transfer", "destroy", "conditional_destroy", "cow_separate",
    "canonicalize",
}
BARRIER_IDS = {
    "safepoint", "reentrancy", "destructor", "exception", "bailout",
    "observer", "interrupt", "suspend",
}
TRISTATE = {"never", "conditional", "always"}
JIT_SUPPORT = {"none", "function", "trace", "both", "partial"}
TARGET_STATES = {"not_started", "planned", "blocked", "not_applicable"}
RESULT_KINDS = {
    "none", "temporary", "in_place", "control", "call_frame", "return",
    "exception", "suspend", "metadata",
}
SPECIAL_CASES = {
    "zts", "generator", "fiber", "exception", "observer", "interrupt",
    "destructor", "reference", "finally", "call", "external_io",
}
SEMANTIC_FIELDS = (
    "result", "may_throw", "may_bailout", "may_call_php", "may_run_dtor",
    "reference_semantics", "observer_boundary", "interrupt_boundary",
    "existing_jit_support", "planned_znmir_lowering", "n0_x64", "n0_a64",
    "special_cases", "effect_ids", "read_domains", "write_domains",
    "ownership_actions", "barrier_ids", "tests", "source_refs", "reason",
)
CSV_FIELDS = (
    "number", "opcode", "operands", "result", "may_throw", "may_bailout",
    "may_call_php", "may_run_dtor", "reference_semantics",
    "observer_boundary", "interrupt_boundary", "existing_jit_support",
    "planned_znmir_lowering", "n0_x64", "n0_a64", "special_cases",
    "effect_ids", "read_domains", "write_domains", "ownership_actions",
    "barrier_ids", "tests", "source_refs", "reason",
)
PROHIBITED_PLACEHOLDER = re.compile(r"\b(?:unknown|tbd|later|probably)\b", re.I)
DEFINE_RE = re.compile(r"^#define\s+(ZEND_[A-Z0-9_]+)\s+([0-9]+)\s*$")
MACRO_RE = re.compile(r"^(ZEND_VM_[A-Z0-9_]+)\((.*)\);?\s*$")
BASE_HANDLER_RE = re.compile(r"^ZEND_VM_(?!.*TYPE_SPEC)[A-Z_]*HANDLER$")
TYPE_HANDLER_RE = re.compile(r"^ZEND_VM_[A-Z_]*TYPE_SPEC_HANDLER$")
DISPATCH_RE = re.compile(
    r"ZEND_VM_DISPATCH_TO_(HANDLER|HELPER)\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)"
)


class MatrixError(Exception):
    """A fail-closed matrix validation error."""


def split_arguments(text: str) -> list[str]:
    parts: list[str] = []
    start = 0
    depth = 0
    for index, char in enumerate(text):
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth < 0:
                raise MatrixError("unbalanced macro arguments")
        elif char == "," and depth == 0:
            parts.append(text[start:index].strip())
            start = index + 1
    if depth != 0:
        raise MatrixError("unbalanced macro arguments")
    parts.append(text[start:].strip())
    return parts


def split_pipe(value: str) -> list[str]:
    return [part.strip() for part in value.split("|") if part.strip()]


def parse_spec(value: str) -> list[str]:
    if not (value.startswith("SPEC(") and value.endswith(")")):
        raise MatrixError("invalid SPEC argument: %s" % value)
    return [part.strip() for part in split_arguments(value[5:-1])]


def read_lines(path: Path) -> list[str]:
    try:
        return path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise MatrixError("cannot read %s: %s" % (path, exc)) from exc


def parse_opcode_header(path: Path) -> tuple[list[dict[str, Any]], int, list[int]]:
    lines = read_lines(path)
    try:
        first = next(i for i, line in enumerate(lines) if line.startswith("#define ZEND_NOP"))
        sentinel_index = next(
            i for i, line in enumerate(lines[first:], first)
            if line.startswith("#define ZEND_VM_LAST_OPCODE")
        )
    except StopIteration as exc:
        raise MatrixError("opcode header lacks ZEND_NOP or ZEND_VM_LAST_OPCODE") from exc

    opcodes: list[dict[str, Any]] = []
    for line_number, line in enumerate(lines[first:sentinel_index], first + 1):
        match = DEFINE_RE.fullmatch(line)
        if match:
            opcodes.append({
                "opcode": match.group(1),
                "number": int(match.group(2)),
                "header_line": line_number,
            })

    sentinel_match = DEFINE_RE.fullmatch(lines[sentinel_index])
    if not sentinel_match or sentinel_match.group(1) != "ZEND_VM_LAST_OPCODE":
        raise MatrixError("invalid ZEND_VM_LAST_OPCODE definition")
    sentinel = int(sentinel_match.group(2))

    names = [item["opcode"] for item in opcodes]
    numbers = [item["number"] for item in opcodes]
    if len(names) != len(set(names)):
        raise MatrixError("duplicate opcode name in canonical header")
    if len(numbers) != len(set(numbers)):
        raise MatrixError("duplicate opcode number in canonical header")
    if not opcodes or names[0] != "ZEND_NOP" or numbers[0] != 0:
        raise MatrixError("canonical opcode range must begin with ZEND_NOP 0")
    if max(numbers) != sentinel:
        raise MatrixError("sentinel does not equal the largest executable opcode")
    holes = sorted(set(range(sentinel + 1)) - set(numbers))
    return opcodes, sentinel, holes


def source_macro_records(lines: list[str]) -> list[tuple[int, str, list[str]]]:
    records: list[tuple[int, str, list[str]]] = []
    for line_number, line in enumerate(lines, 1):
        match = MACRO_RE.fullmatch(line)
        if not match:
            continue
        macro = match.group(1)
        if (BASE_HANDLER_RE.fullmatch(macro) or TYPE_HANDLER_RE.fullmatch(macro)
                or macro == "ZEND_VM_DEFINE_OP" or macro.endswith("_HELPER")):
            records.append((line_number, macro, split_arguments(match.group(2))))
    return records


def parse_vm_def(path: Path) -> dict[str, dict[str, Any]]:
    lines = read_lines(path)
    records = source_macro_records(lines)
    handlers: dict[str, dict[str, Any]] = {}
    number_to_name: dict[int, str] = {}
    variants: list[tuple[int, str, list[str]]] = []
    helpers: dict[str, dict[str, int]] = {}

    for record_index, (line_number, macro, args) in enumerate(records):
        if not macro.endswith("_HELPER"):
            continue
        if not args:
            raise MatrixError("invalid helper at %s:%d" % (path, line_number))
        name = args[0]
        if name in helpers:
            raise MatrixError("duplicate helper definition for %s" % name)
        next_line = records[record_index + 1][0] if record_index + 1 < len(records) else len(lines) + 1
        helpers[name] = {"start_line": line_number, "end_line": next_line - 1}

    for record_index, (line_number, macro, args) in enumerate(records):
        next_line = records[record_index + 1][0] if record_index + 1 < len(records) else len(lines) + 1
        if TYPE_HANDLER_RE.fullmatch(macro):
            variants.append((line_number, macro, args))
            continue
        if macro == "ZEND_VM_DEFINE_OP":
            if len(args) != 2 or not args[0].isdigit():
                raise MatrixError("invalid ZEND_VM_DEFINE_OP at %s:%d" % (path, line_number))
            number = int(args[0])
            name = args[1]
            operands = {
                "handler_style": macro,
                "op1": ["ANY"],
                "op2": ["ANY"],
                "extended": [],
                "spec": [],
                "variants": [],
                "dispatches": [],
            }
        elif BASE_HANDLER_RE.fullmatch(macro):
            if len(args) < 4 or not args[0].isdigit():
                raise MatrixError("invalid base handler at %s:%d" % (path, line_number))
            number = int(args[0])
            name = args[1]
            trailing = args[4:]
            spec: list[str] = []
            extended: list[str] = []
            for value in trailing:
                if value.startswith("SPEC("):
                    spec.extend(parse_spec(value))
                else:
                    extended.extend(split_pipe(value))
            operands = {
                "handler_style": macro,
                "op1": split_pipe(args[2]),
                "op2": split_pipe(args[3]),
                "extended": extended,
                "spec": spec,
                "variants": [],
                "dispatches": [],
            }
        else:
            continue

        if name in handlers:
            raise MatrixError("duplicate base handler for %s" % name)
        if number in number_to_name:
            raise MatrixError("duplicate base handler number %d" % number)
        handlers[name] = {
            "number": number,
            "operands": operands,
            "handler_start": line_number,
            "handler_end": next_line - 1,
        }
        number_to_name[number] = name

    for line_number, macro, args in variants:
        if len(args) < 5:
            raise MatrixError("invalid type-specialized handler at %s:%d" % (path, line_number))
        original_names = split_pipe(args[0])
        trailing = args[5:]
        spec: list[str] = []
        extended: list[str] = []
        for value in trailing:
            if value.startswith("SPEC("):
                spec.extend(parse_spec(value))
            else:
                extended.extend(split_pipe(value))
        variant = {
            "handler_style": macro,
            "condition": args[1],
            "name": args[2],
            "op1": split_pipe(args[3]),
            "op2": split_pipe(args[4]),
            "extended": extended,
            "spec": spec,
            "source_line": line_number,
        }
        for name in original_names:
            if name not in handlers:
                raise MatrixError("variant references undefined opcode %s" % name)
            handlers[name]["operands"]["variants"].append(variant)

    for handler in handlers.values():
        handler["operands"]["variants"].sort(
            key=lambda item: (item["source_line"], item["name"])
        )

        seen_dispatches: set[tuple[str, str]] = set()
        for source_line in range(handler["handler_start"], handler["handler_end"] + 1):
            for match in DISPATCH_RE.finditer(lines[source_line - 1]):
                kind = match.group(1).lower()
                target = match.group(2)
                key = (kind, target)
                if key in seen_dispatches:
                    continue
                seen_dispatches.add(key)
                if kind == "handler":
                    if target not in handlers:
                        raise MatrixError("dispatch references undefined handler %s" % target)
                    target_record = handlers[target]
                    target_start = target_record["handler_start"]
                    target_end = target_record["handler_end"]
                else:
                    if target not in helpers:
                        raise MatrixError("dispatch references undefined helper %s" % target)
                    target_start = helpers[target]["start_line"]
                    target_end = helpers[target]["end_line"]
                handler["operands"]["dispatches"].append({
                    "kind": kind,
                    "target": target,
                    "source_line": source_line,
                    "target_start_line": target_start,
                    "target_end_line": target_end,
                })
        handler["operands"]["dispatches"].sort(
            key=lambda item: (item["source_line"], item["kind"], item["target"])
        )
    return handlers


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise MatrixError("cannot load JSON %s: %s" % (path, exc)) from exc


def require_string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise MatrixError("%s must be a non-empty string" % label)
    if PROHIBITED_PLACEHOLDER.search(value):
        raise MatrixError("%s contains a prohibited placeholder" % label)
    return value


def require_string_list(value: Any, label: str, allowed: set[str] | None = None) -> list[str]:
    if not isinstance(value, list) or any(not isinstance(item, str) or not item for item in value):
        raise MatrixError("%s must be a string array" % label)
    if len(value) != len(set(value)):
        raise MatrixError("%s contains duplicates" % label)
    for item in value:
        if PROHIBITED_PLACEHOLDER.search(item):
            raise MatrixError("%s contains a prohibited placeholder" % label)
        if allowed is not None and item not in allowed:
            raise MatrixError("%s contains invalid value %s" % (label, item))
    return value


def validate_source_ref(repo_root: Path, ref: Any, label: str) -> dict[str, Any]:
    if not isinstance(ref, dict) or set(ref) != {"path", "symbol", "start_line", "end_line"}:
        raise MatrixError("%s has invalid keys" % label)
    rel_path = require_string(ref["path"], label + ".path")
    relative = Path(rel_path)
    if relative.is_absolute() or ".." in relative.parts:
        raise MatrixError("%s.path must be repository-relative" % label)
    symbol = require_string(ref["symbol"], label + ".symbol")
    start = ref["start_line"]
    end = ref["end_line"]
    if not isinstance(start, int) or not isinstance(end, int) or start < 1 or end < start:
        raise MatrixError("%s has an invalid line range" % label)
    lines = read_lines(repo_root / relative)
    if end > len(lines):
        raise MatrixError("%s line range exceeds %s" % (label, rel_path))
    if not any(symbol in line for line in lines[start - 1:end]):
        raise MatrixError("%s symbol is absent from its line range" % label)
    return {"path": rel_path, "symbol": symbol, "start_line": start, "end_line": end}


def validate_override(repo_root: Path, opcode: str, value: Any,
                      dispatches: list[dict[str, Any]]) -> dict[str, Any]:
    label = "override[%s]" % opcode
    if not isinstance(value, dict):
        raise MatrixError("%s must be an object" % label)
    missing = set(SEMANTIC_FIELDS) - set(value)
    extra = set(value) - set(SEMANTIC_FIELDS)
    if missing or extra:
        raise MatrixError("%s field mismatch: missing=%s extra=%s" % (
            label, sorted(missing), sorted(extra)))

    result = require_string(value["result"], label + ".result")
    if result not in RESULT_KINDS:
        raise MatrixError("%s.result is invalid" % label)
    for field in ("may_throw", "may_bailout", "may_call_php", "may_run_dtor",
                  "observer_boundary", "interrupt_boundary"):
        if value[field] not in TRISTATE:
            raise MatrixError("%s.%s is invalid" % (label, field))
    if value["existing_jit_support"] not in JIT_SUPPORT:
        raise MatrixError("%s.existing_jit_support is invalid" % label)
    for field in ("n0_x64", "n0_a64"):
        if value[field] not in TARGET_STATES:
            raise MatrixError("%s.%s is invalid" % (label, field))

    require_string(value["reference_semantics"], label + ".reference_semantics")
    family = require_string(value["planned_znmir_lowering"], label + ".planned_znmir_lowering")
    if not re.fullmatch(r"[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+", family):
        raise MatrixError("%s has an invalid lowering family" % label)
    reason = require_string(value["reason"], label + ".reason")

    for field, allowed in (
        ("special_cases", SPECIAL_CASES),
        ("effect_ids", EFFECT_IDS),
        ("read_domains", MEMORY_DOMAINS),
        ("write_domains", MEMORY_DOMAINS),
        ("ownership_actions", OWNERSHIP_ACTIONS),
        ("barrier_ids", BARRIER_IDS),
    ):
        require_string_list(value[field], label + "." + field, allowed)
    require_string_list(value["tests"], label + ".tests")
    if not value["tests"]:
        raise MatrixError("%s.tests must not be empty" % label)
    if not value["source_refs"]:
        raise MatrixError("%s.source_refs must not be empty" % label)
    refs = [
        validate_source_ref(repo_root, ref, "%s.source_refs[%d]" % (label, index))
        for index, ref in enumerate(value["source_refs"])
    ]
    if not any(ref["path"] == "Zend/zend_vm_def.h" and ref["symbol"] == opcode for ref in refs):
        raise MatrixError("%s lacks its canonical VM-definition source reference" % label)
    for dispatch in dispatches:
        if not any(
            ref["path"] == "Zend/zend_vm_def.h"
            and ref["symbol"] == dispatch["target"]
            and ref["start_line"] <= dispatch["target_start_line"]
            and ref["end_line"] >= dispatch["target_end_line"]
            for ref in refs
        ):
            raise MatrixError("%s lacks dispatched %s source reference %s" % (
                label, dispatch["kind"], dispatch["target"]))
    if value["existing_jit_support"] != "none" \
            and not any(ref["path"].startswith("ext/opcache/jit/") for ref in refs):
        raise MatrixError("%s claims JIT support without a JIT source reference" % label)

    effects = set(value["effect_ids"])
    if value["read_domains"] and "read_memory" not in effects:
        raise MatrixError("%s reads memory without read_memory" % label)
    if value["write_domains"] and "write_memory" not in effects:
        raise MatrixError("%s writes memory without write_memory" % label)
    for field, effect in (
        ("may_throw", "throw"), ("may_bailout", "bailout"),
        ("may_call_php", "call_php"), ("may_run_dtor", "run_destructor"),
        ("observer_boundary", "observe_frame"),
        ("interrupt_boundary", "interrupt_boundary"),
    ):
        if value[field] != "never" and effect not in effects:
            raise MatrixError("%s.%s lacks effect %s" % (label, field, effect))
    required_barriers = {
        "may_throw": "exception", "may_bailout": "bailout",
        "may_call_php": "reentrancy", "may_run_dtor": "destructor",
        "observer_boundary": "observer", "interrupt_boundary": "interrupt",
    }
    for field, barrier in required_barriers.items():
        if value[field] != "never" and barrier not in value["barrier_ids"]:
            raise MatrixError("%s.%s lacks barrier %s" % (label, field, barrier))
    if (value["may_run_dtor"] != "never" or value["may_call_php"] != "never") \
            and not value["ownership_actions"]:
        raise MatrixError("%s has a call/destructor path without ownership actions" % label)
    effect_barriers = {
        "run_destructor": "destructor",
        "call_php": "reentrancy",
        "bailout": "bailout",
        "suspend": "suspend",
    }
    for effect, barrier in effect_barriers.items():
        if effect in effects:
            if barrier not in value["barrier_ids"]:
                raise MatrixError("%s effect %s lacks barrier %s" % (label, effect, barrier))
            if not value["ownership_actions"]:
                raise MatrixError("%s effect %s lacks ownership actions" % (label, effect))
    if not value["read_domains"] and not value["write_domains"] \
            and not value["ownership_actions"] \
            and "pure/no-memory/no-ownership" not in reason:
        raise MatrixError("%s empty memory/ownership requires an explicit pure justification" % label)

    normalized = dict(value)
    normalized["source_refs"] = sorted(
        refs, key=lambda ref: (ref["path"], ref["start_line"], ref["end_line"], ref["symbol"])
    )
    for field in ("special_cases", "effect_ids", "read_domains", "write_domains",
                  "ownership_actions", "barrier_ids", "tests"):
        normalized[field] = sorted(value[field])
    return normalized


def file_hash(path: Path) -> str:
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError as exc:
        raise MatrixError("cannot hash %s: %s" % (path, exc)) from exc


def build_matrix(repo_root: Path, opcodes_path: Path, vm_def_path: Path,
                 overrides_path: Path) -> dict[str, Any]:
    opcodes, sentinel, holes = parse_opcode_header(opcodes_path)
    handlers = parse_vm_def(vm_def_path)
    canonical_names = {item["opcode"] for item in opcodes}
    if canonical_names != set(handlers):
        raise MatrixError("handler coverage mismatch: missing=%s extra=%s" % (
            sorted(canonical_names - set(handlers)), sorted(set(handlers) - canonical_names)))
    for item in opcodes:
        handler = handlers[item["opcode"]]
        if item["number"] != handler["number"]:
            raise MatrixError("handler/header number mismatch for %s" % item["opcode"])

    override_document = load_json(overrides_path)
    if not isinstance(override_document, dict) \
            or set(override_document) != {"format_version", "source_commit", "opcodes"}:
        raise MatrixError("override document has invalid top-level fields")
    if override_document["format_version"] != FORMAT_VERSION:
        raise MatrixError("unsupported override format version")
    if override_document["source_commit"] != SOURCE_COMMIT:
        raise MatrixError("override source commit is stale")
    overrides = override_document["opcodes"]
    if not isinstance(overrides, dict):
        raise MatrixError("override opcodes must be an object")
    override_names = set(overrides)
    if override_names != canonical_names:
        raise MatrixError("override coverage mismatch: missing=%s extra=%s" % (
            sorted(canonical_names - override_names), sorted(override_names - canonical_names)))

    rows: list[dict[str, Any]] = []
    for item in sorted(opcodes, key=lambda entry: (entry["number"], entry["opcode"])):
        opcode = item["opcode"]
        semantic = validate_override(
            repo_root, opcode, overrides[opcode], handlers[opcode]["operands"]["dispatches"]
        )
        row = {
            "opcode": opcode,
            "number": item["number"],
            "operands": handlers[opcode]["operands"],
        }
        row.update({field: semantic[field] for field in SEMANTIC_FIELDS})
        rows.append(row)

    return {
        "format_version": FORMAT_VERSION,
        "source_commit": SOURCE_COMMIT,
        "source_files": [
            {"path": rel_path, "sha256": file_hash(repo_root / rel_path)}
            for rel_path in SOURCE_PATHS
        ],
        "sentinel": {"name": "ZEND_VM_LAST_OPCODE", "value": sentinel},
        "reserved_opcode_numbers": holes,
        "opcodes": rows,
    }


def json_bytes(value: Any) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n").encode("utf-8")


def csv_bytes(rows: Iterable[dict[str, Any]]) -> bytes:
    stream = io.StringIO(newline="")
    writer = csv.DictWriter(stream, fieldnames=CSV_FIELDS, lineterminator="\n")
    writer.writeheader()
    for row in rows:
        encoded: dict[str, Any] = {}
        for field in CSV_FIELDS:
            value = row[field]
            if isinstance(value, (dict, list)):
                encoded[field] = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
            else:
                encoded[field] = value
        writer.writerow(encoded)
    return stream.getvalue().encode("utf-8")


def output_payloads(matrix: dict[str, Any]) -> dict[str, bytes]:
    return {
        "opcode-matrix.json": json_bytes(matrix),
        "opcode-matrix.csv": csv_bytes(matrix["opcodes"]),
    }


def write_outputs(output_dir: Path, payloads: dict[str, bytes]) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    for name, payload in payloads.items():
        (output_dir / name).write_bytes(payload)


def check_outputs(output_dir: Path, payloads: dict[str, bytes]) -> None:
    mismatches: list[str] = []
    for name, payload in payloads.items():
        path = output_dir / name
        try:
            actual = path.read_bytes()
        except OSError:
            mismatches.append(name + " (missing)")
            continue
        if actual != payload:
            mismatches.append(name + " (stale)")
    if mismatches:
        raise MatrixError("checked-in generator output differs: %s" % ", ".join(mismatches))


def default_repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=default_repo_root())
    parser.add_argument("--opcodes-header", type=Path)
    parser.add_argument("--vm-def", type=Path)
    parser.add_argument("--overrides", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--check", action="store_true",
                        help="compare generated JSON and CSV with checked-in outputs")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    repo_root = args.repo_root.resolve()
    output_dir = args.output_dir or repo_root / "docs/native-engine/semantics/opcodes"
    opcodes_path = args.opcodes_header or repo_root / "Zend/zend_vm_opcodes.h"
    vm_def_path = args.vm_def or repo_root / "Zend/zend_vm_def.h"
    overrides_path = args.overrides or output_dir / "opcode-overrides.json"
    try:
        matrix = build_matrix(repo_root, opcodes_path, vm_def_path, overrides_path)
        payloads = output_payloads(matrix)
        if args.check:
            check_outputs(output_dir, payloads)
            print("opcode matrix is complete and up to date (%d opcodes)" % len(matrix["opcodes"]))
        else:
            write_outputs(output_dir, payloads)
            print("generated opcode matrix (%d opcodes) in %s" % (len(matrix["opcodes"]), output_dir))
        return 0
    except MatrixError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
