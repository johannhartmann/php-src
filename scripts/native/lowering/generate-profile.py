#!/usr/bin/env python3
"""Generate and validate the W03 opcode lowering profile."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[3]
MATRIX = ROOT / "docs/native-engine/semantics/opcodes/opcode-matrix.json"
PROFILE = ROOT / "docs/native-engine/lowering/w03-opcode-profile.json"

COMMON_PROOFS = (
    "single_reachable_block",
    "no_calls",
    "no_reentry",
    "exact_scalar_types",
)

PROOF_CATALOG = {
    *COMMON_PROOFS,
    "no_overflow",
    "nonzero_divisor",
    "valid_shift_count",
    "non_refcounted",
    "same_exact_type",
    "finite_f64",
    "safe_scalar_cast",
    "not_return_by_reference",
    "no_observer",
    "no_destructor",
    "no_exception",
}

DECISIONS: dict[str, dict[str, Any]] = {
    "ZEND_NOP": {
        "classification": "required",
        "provider": "core",
        "proofs": ["single_reachable_block", "no_calls", "no_reentry"],
        "mir_opcodes": [],
        "rationale": "The cited handler is an effect-free metadata no-op.",
    },
    "ZEND_QM_ASSIGN": {
        "provider": "frontend",
        "proofs": [*COMMON_PROOFS, "non_refcounted"],
        "mir_opcodes": ["copy"],
    },
    "ZEND_ADD": {
        "provider": "numeric",
        "proofs": [*COMMON_PROOFS, "no_overflow", "no_destructor", "no_exception"],
        "mir_opcodes": ["i64_add_no_overflow", "f64_add"],
    },
    "ZEND_SUB": {
        "provider": "numeric",
        "proofs": [*COMMON_PROOFS, "no_overflow", "no_destructor", "no_exception"],
        "mir_opcodes": ["i64_sub_no_overflow", "f64_sub"],
    },
    "ZEND_MUL": {
        "provider": "numeric",
        "proofs": [*COMMON_PROOFS, "no_overflow", "no_destructor", "no_exception"],
        "mir_opcodes": ["i64_mul_no_overflow", "f64_mul"],
    },
    "ZEND_MOD": {
        "provider": "numeric",
        "proofs": [*COMMON_PROOFS, "nonzero_divisor", "no_overflow", "no_exception"],
        "mir_opcodes": ["i64_mod_nonzero"],
    },
    "ZEND_SL": {
        "provider": "numeric",
        "proofs": [*COMMON_PROOFS, "valid_shift_count", "no_overflow"],
        "mir_opcodes": ["i64_shl_checked"],
    },
    "ZEND_SR": {
        "provider": "numeric",
        "proofs": [*COMMON_PROOFS, "valid_shift_count"],
        "mir_opcodes": ["i64_shr_checked"],
    },
    "ZEND_BW_OR": {
        "provider": "numeric",
        "proofs": list(COMMON_PROOFS),
        "mir_opcodes": ["i64_bit_or"],
    },
    "ZEND_BW_AND": {
        "provider": "numeric",
        "proofs": list(COMMON_PROOFS),
        "mir_opcodes": ["i64_bit_and"],
    },
    "ZEND_BW_XOR": {
        "provider": "numeric",
        "proofs": list(COMMON_PROOFS),
        "mir_opcodes": ["i64_bit_xor"],
    },
    "ZEND_BW_NOT": {
        "provider": "numeric",
        "proofs": list(COMMON_PROOFS),
        "mir_opcodes": ["i64_bit_not"],
    },
    "ZEND_BOOL": {
        "provider": "logic",
        "proofs": [*COMMON_PROOFS, "safe_scalar_cast"],
        "mir_opcodes": ["i64_to_i1", "f64_to_i1"],
    },
    "ZEND_BOOL_NOT": {
        "provider": "logic",
        "proofs": list(COMMON_PROOFS),
        "mir_opcodes": ["i1_not"],
    },
    "ZEND_BOOL_XOR": {
        "provider": "logic",
        "proofs": list(COMMON_PROOFS),
        "mir_opcodes": ["i1_xor"],
    },
    "ZEND_CAST": {
        "provider": "logic",
        "proofs": [*COMMON_PROOFS, "safe_scalar_cast", "finite_f64", "no_destructor", "no_exception"],
        "mir_opcodes": [
            "i64_to_f64",
            "f64_to_i64_checked",
            "i64_to_i1",
            "f64_to_i1",
            "i1_to_i64",
            "i1_to_f64",
        ],
    },
    "ZEND_FREE": {
        "provider": "lifetime",
        "proofs": [*COMMON_PROOFS, "non_refcounted", "no_destructor"],
        "mir_opcodes": ["scalar_drop"],
    },
    "ZEND_RETURN": {
        "provider": "lifetime",
        "proofs": [
            *COMMON_PROOFS,
            "non_refcounted",
            "not_return_by_reference",
            "no_observer",
            "no_destructor",
            "no_exception",
        ],
        "mir_opcodes": ["return"],
    },
}

for name, opcodes in {
    "ZEND_IS_IDENTICAL": ["i64_eq", "f64_eq", "i1_eq"],
    "ZEND_IS_NOT_IDENTICAL": ["i64_eq", "f64_eq", "i1_eq", "i1_not"],
    "ZEND_IS_EQUAL": ["i64_eq", "f64_eq", "i1_eq"],
    "ZEND_IS_NOT_EQUAL": ["i64_eq", "f64_eq", "i1_eq", "i1_not"],
    "ZEND_IS_SMALLER": ["i64_lt", "f64_lt"],
    "ZEND_IS_SMALLER_OR_EQUAL": ["i64_le", "f64_le"],
    "ZEND_SPACESHIP": ["i64_cmp", "f64_cmp"],
}.items():
    DECISIONS[name] = {
        "provider": "logic",
        "proofs": [*COMMON_PROOFS, "same_exact_type", "finite_f64"],
        "mir_opcodes": opcodes,
    }

DEFERRED_WAVE_BY_FAMILY = {
    "control.branch": "W04",
    "control.return": "W04",
    "call.dynamic": "W05",
    "runtime.declaration": "W05",
    "runtime.exception": "W05",
    "runtime.misc": "W06",
    "runtime.reference": "W06",
    "runtime.variable": "W06",
    "runtime.array": "W07",
    "runtime.object": "W08",
    "suspend.generator": "W11",
    "scalar.binary": "W06",
    "scalar.compare": "W06",
}


def _source_snapshot(opcode: dict[str, Any]) -> dict[str, Any]:
    return {
        "planned_znmir_lowering": opcode["planned_znmir_lowering"],
        "effect_ids": opcode["effect_ids"],
        "barrier_ids": opcode["barrier_ids"],
        "ownership_actions": opcode["ownership_actions"],
        "may_bailout": opcode["may_bailout"],
        "may_call_php": opcode["may_call_php"],
        "may_run_dtor": opcode["may_run_dtor"],
        "may_throw": opcode["may_throw"],
        "observer_boundary": opcode["observer_boundary"],
        "interrupt_boundary": opcode["interrupt_boundary"],
    }


def build_profile(matrix: dict[str, Any]) -> dict[str, Any]:
    entries = []
    for opcode in sorted(matrix["opcodes"], key=lambda item: item["number"]):
        decision = DECISIONS.get(opcode["opcode"])
        if decision is None:
            family = opcode["planned_znmir_lowering"]
            entries.append(
                {
                    "number": opcode["number"],
                    "opcode": opcode["opcode"],
                    "classification": "deferred",
                    "provider": None,
                    "deferred_wave": DEFERRED_WAVE_BY_FAMILY[family],
                    "proofs": [],
                    "mir_opcodes": [],
                    "rationale": "Outside the proof-closed straight-line scalar subset.",
                    "w01": _source_snapshot(opcode),
                    "source_refs": opcode["source_refs"],
                }
            )
            continue
        entry = {
            "number": opcode["number"],
            "opcode": opcode["opcode"],
            "classification": decision.get("classification", "conditional"),
            "provider": decision["provider"],
            "deferred_wave": None,
            "proofs": decision["proofs"],
            "mir_opcodes": decision["mir_opcodes"],
            "rationale": decision.get(
                "rationale",
                "Accepted only when every named proof excludes the conservative W01 hazards.",
            ),
            "w01": _source_snapshot(opcode),
            "source_refs": opcode["source_refs"],
        }
        entries.append(entry)
    return {
        "format_version": 1,
        "mir_contract_version": "1.2",
        "source_commit": matrix["source_commit"],
        "source_matrix": "docs/native-engine/semantics/opcodes/opcode-matrix.json",
        "active_opcode_count": len(entries),
        "reserved_opcode_numbers": matrix["reserved_opcode_numbers"],
        "proof_catalog": sorted(PROOF_CATALOG),
        "opcodes": entries,
    }


def validate_profile(profile: dict[str, Any], matrix: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    entries = profile.get("opcodes", [])
    if profile.get("active_opcode_count") != len(matrix["opcodes"]):
        errors.append("active_opcode_count does not match the W01 matrix")
    if len(entries) != len(matrix["opcodes"]):
        errors.append("profile must classify every active W01 opcode exactly once")
    numbers = [entry.get("number") for entry in entries]
    names = [entry.get("opcode") for entry in entries]
    if len(numbers) != len(set(numbers)) or len(names) != len(set(names)):
        errors.append("profile contains duplicate opcode identities")
    if numbers != [opcode["number"] for opcode in matrix["opcodes"]]:
        errors.append("profile opcode order/identity drifted from the W01 matrix")
    known_proofs = set(profile.get("proof_catalog", []))
    for entry in entries:
        classification = entry.get("classification")
        proofs = entry.get("proofs", [])
        if classification not in {"required", "conditional", "deferred"}:
            errors.append(f"{entry.get('opcode')}: unknown classification")
        if classification == "conditional" and not proofs:
            errors.append(f"{entry.get('opcode')}: conditional entry has no proofs")
        if classification == "deferred" and not entry.get("deferred_wave"):
            errors.append(f"{entry.get('opcode')}: deferred entry has no later wave")
        unknown = sorted(set(proofs) - known_proofs)
        if unknown:
            errors.append(f"{entry.get('opcode')}: unknown proofs {unknown}")
        if not entry.get("source_refs"):
            errors.append(f"{entry.get('opcode')}: missing W01 source references")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    args = parser.parse_args()

    matrix = json.loads(MATRIX.read_text())
    generated = build_profile(matrix)
    errors = validate_profile(generated, matrix)
    if errors:
        for error in errors:
            print(f"error: {error}")
        return 1
    rendered = json.dumps(generated, indent=2, sort_keys=False) + "\n"
    if args.write:
        PROFILE.parent.mkdir(parents=True, exist_ok=True)
        PROFILE.write_text(rendered)
        print(f"wrote {PROFILE.relative_to(ROOT)} ({len(generated['opcodes'])} opcodes)")
        return 0
    if not PROFILE.exists() or PROFILE.read_text() != rendered:
        print("error: w03-opcode-profile.json is stale; run generate-profile.py --write")
        return 1
    print(f"W03 profile valid: {len(generated['opcodes'])} active opcodes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
