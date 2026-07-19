#!/usr/bin/env python3
"""Generate and validate the source-backed W04 opcode profile."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
from typing import Any

ROOT = Path(__file__).resolve().parents[3]
W01_MATRIX = ROOT / "docs/native-engine/semantics/opcodes/opcode-matrix.json"
W03_PROFILE = ROOT / "docs/native-engine/lowering/w03-opcode-profile.json"
W04_PROFILE = ROOT / "docs/native-engine/control-flow/w04-opcode-profile.json"

W04_OWNER = "W04-A-production-control-flow"

CFG_PROOFS = (
    "source_cfg_complete",
    "reducible_cfg",
    "no_protected_regions",
    "branch_successor_order_exact",
    "phi_predecessor_order_exact",
    "edge_statepoints_complete",
)

PROOF_CATALOG = {
    *CFG_PROOFS,
    "condition_truthiness_side_effect_free",
    "exact_scalar_types",
    "finite_f64",
    "no_bailout",
    "no_calls",
    "no_destructor",
    "no_exception",
    "no_observer",
    "no_overflow",
    "no_reentry",
    "non_refcounted",
    "nonzero_divisor",
    "not_return_by_reference",
    "result_ssa_complete",
    "safe_scalar_cast",
    "same_exact_type",
    "valid_shift_count",
}

W04_DECISIONS: dict[str, dict[str, Any]] = {
    "ZEND_JMP": {
        "classification": "required",
        "proofs": list(CFG_PROOFS),
        "mir_opcodes": ["branch"],
        "rationale": "Accepted when the source edge, target block, and edge statepoint map exactly.",
    },
    "ZEND_JMPZ": {
        "classification": "conditional",
        "proofs": [
            *CFG_PROOFS,
            "exact_scalar_types",
            "condition_truthiness_side_effect_free",
            "no_calls",
            "no_bailout",
            "no_destructor",
            "no_exception",
            "no_observer",
            "no_reentry",
            "non_refcounted",
        ],
        "mir_opcodes": ["cond_branch"],
        "rationale": (
            "Source successor 0 is the false target and successor 1 is the true "
            "fallthrough; MIR successor 0 is true and successor 1 is false."
        ),
    },
    "ZEND_JMPNZ": {
        "classification": "conditional",
        "proofs": [
            *CFG_PROOFS,
            "exact_scalar_types",
            "condition_truthiness_side_effect_free",
            "no_calls",
            "no_bailout",
            "no_destructor",
            "no_exception",
            "no_observer",
            "no_reentry",
            "non_refcounted",
        ],
        "mir_opcodes": ["cond_branch"],
        "rationale": (
            "Source successor 0 is the true target and successor 1 is the false "
            "fallthrough; MIR uses the same true/false order."
        ),
    },
    "ZEND_JMPZ_EX": {
        "classification": "conditional",
        "proofs": [
            *CFG_PROOFS,
            "exact_scalar_types",
            "condition_truthiness_side_effect_free",
            "result_ssa_complete",
            "no_calls",
            "no_bailout",
            "no_destructor",
            "no_exception",
            "no_observer",
            "no_reentry",
            "non_refcounted",
        ],
        "mir_opcodes": ["copy", "cond_branch"],
        "rationale": "JMPZ mapping plus an exact source-backed result SSA definition.",
    },
    "ZEND_JMPNZ_EX": {
        "classification": "conditional",
        "proofs": [
            *CFG_PROOFS,
            "exact_scalar_types",
            "condition_truthiness_side_effect_free",
            "result_ssa_complete",
            "no_calls",
            "no_bailout",
            "no_destructor",
            "no_exception",
            "no_observer",
            "no_reentry",
            "non_refcounted",
        ],
        "mir_opcodes": ["copy", "cond_branch"],
        "rationale": "JMPNZ mapping plus an exact source-backed result SSA definition.",
    },
}

LATER_WAVE_DECISIONS = {
    "ZEND_RETURN_BY_REF": ("W06", "Reference and return-by-reference ownership semantics."),
    "ZEND_ASSERT_CHECK": ("W05", "Dynamic call and assertion control semantics."),
    "ZEND_JMP_SET": ("W06", "Reference/COW and value-selection semantics."),
    "ZEND_GENERATOR_RETURN": ("W11", "Generator suspension and completion semantics."),
    "ZEND_FAST_CALL": ("W09", "Protected-region and finally control flow."),
    "ZEND_FAST_RET": ("W09", "Protected-region and finally control flow."),
    "ZEND_COALESCE": ("W06", "Reference/COW and value-selection semantics."),
    "ZEND_SWITCH_LONG": ("W07", "Multi-way branch proof is not source-closed in W04."),
    "ZEND_SWITCH_STRING": ("W07", "String dispatch and multi-way branch semantics."),
    "ZEND_MATCH": ("W07", "Match dispatch and exception semantics."),
    "ZEND_JMP_NULL": ("W06", "Null-chain value and reference semantics."),
    "ZEND_BIND_INIT_STATIC_OR_JMP": ("W06", "Static-variable binding and COW semantics."),
    "ZEND_JMP_FRAMELESS": ("W05", "Frameless call semantics."),
}


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _replace_straight_line_proof(proofs: list[str]) -> list[str]:
    result: list[str] = []
    for proof in proofs:
        if proof == "single_reachable_block":
            for cfg_proof in CFG_PROOFS:
                if cfg_proof not in result:
                    result.append(cfg_proof)
        elif proof not in result:
            result.append(proof)
    return result


def build_profile(
    matrix: dict[str, Any],
    w03: dict[str, Any],
    *,
    matrix_sha256: str,
    w03_sha256: str,
) -> dict[str, Any]:
    w01_by_number = {entry["number"]: entry for entry in matrix["opcodes"]}
    entries: list[dict[str, Any]] = []
    for old in w03["opcodes"]:
        source = w01_by_number[old["number"]]
        if source["opcode"] != old["opcode"]:
            raise ValueError(f"W01/W03 opcode identity mismatch at {old['number']}")

        name = old["opcode"]
        if old["classification"] != "deferred":
            entry = dict(old)
            entry["owner"] = W04_OWNER
            entry["provider_owner"] = old["owner"]
            entry["proofs"] = _replace_straight_line_proof(old["proofs"])
            entry["rationale"] = (
                f"Inherited from {old['owner']} with the W04 source-CFG proof set "
                "replacing the W03 single-block restriction."
            )
        elif old.get("deferred_wave") == "W04":
            decision = W04_DECISIONS.get(name)
            later = LATER_WAVE_DECISIONS.get(name)
            if decision is not None:
                entry = {
                    "number": old["number"],
                    "opcode": name,
                    "classification": decision["classification"],
                    "owner": W04_OWNER,
                    "provider_owner": None,
                    "deferred_wave": None,
                    "proofs": decision["proofs"],
                    "mir_opcodes": decision["mir_opcodes"],
                    "rationale": decision["rationale"],
                    "w01": old["w01"],
                    "source_refs": old["source_refs"],
                }
            elif later is not None:
                wave, rationale = later
                entry = dict(old)
                entry["owner"] = "W04-integration-gate"
                entry["provider_owner"] = None
                entry["deferred_wave"] = wave
                entry["rationale"] = rationale
            else:
                raise ValueError(f"unresolved W04-deferred opcode: {name}")
        else:
            entry = dict(old)
            entry["provider_owner"] = None
        entries.append(entry)

    return {
        "format_version": 1,
        "mir_contract_version": "1.3",
        "sources": {
            "w01": {
                "path": W01_MATRIX.relative_to(ROOT).as_posix(),
                "sha256": matrix_sha256,
                "source_commit": matrix["source_commit"],
                "active_opcode_count": len(matrix["opcodes"]),
            },
            "w03": {
                "path": W03_PROFILE.relative_to(ROOT).as_posix(),
                "sha256": w03_sha256,
                "source_commit": w03["source_commit"],
                "active_opcode_count": len(w03["opcodes"]),
            },
        },
        "active_opcode_count": len(entries),
        "reserved_opcode_numbers": matrix["reserved_opcode_numbers"],
        "proof_catalog": sorted(PROOF_CATALOG),
        "opcodes": entries,
    }


def validate_profile(
    profile: dict[str, Any],
    matrix: dict[str, Any],
    w03: dict[str, Any],
    *,
    matrix_sha256: str,
    w03_sha256: str,
) -> list[str]:
    errors: list[str] = []
    try:
        expected = build_profile(
            matrix,
            w03,
            matrix_sha256=matrix_sha256,
            w03_sha256=w03_sha256,
        )
    except (KeyError, TypeError, ValueError) as error:
        return [str(error)]

    if profile != expected:
        errors.append("profile differs from live W01/W03-derived output")
    entries = profile.get("opcodes", [])
    if profile.get("active_opcode_count") != len(matrix.get("opcodes", [])):
        errors.append("active_opcode_count does not match the live W01 matrix")
    if len(entries) != len(w03.get("opcodes", [])):
        errors.append("profile does not classify every live W03 opcode")
    identities = [(entry.get("number"), entry.get("opcode")) for entry in entries]
    expected_identities = [
        (entry["number"], entry["opcode"]) for entry in matrix.get("opcodes", [])
    ]
    if identities != expected_identities:
        errors.append("profile opcode order or identity differs from W01")
    if len(identities) != len(set(identities)):
        errors.append("profile contains duplicate opcode identities")

    known_proofs = set(profile.get("proof_catalog", []))
    for entry in entries:
        name = entry.get("opcode", "<unknown>")
        classification = entry.get("classification")
        proofs = entry.get("proofs", [])
        if classification not in {"required", "conditional", "deferred"}:
            errors.append(f"{name}: invalid classification")
        if classification == "deferred" and not entry.get("deferred_wave"):
            errors.append(f"{name}: deferred entry has no explicit later wave")
        if classification != "deferred" and entry.get("deferred_wave") is not None:
            errors.append(f"{name}: accepted entry has deferred_wave")
        if classification == "conditional" and not proofs:
            errors.append(f"{name}: conditional entry has no proofs")
        if "single_reachable_block" in proofs:
            errors.append(f"{name}: W03 single-block proof remains")
        unknown = sorted(set(proofs) - known_proofs)
        if unknown:
            errors.append(f"{name}: unknown proofs {unknown}")
        if entry.get("deferred_wave") == "W04":
            errors.append(f"{name}: unresolved W04 deferral")

    for name in W04_DECISIONS:
        entry = next((item for item in entries if item.get("opcode") == name), None)
        if entry is None or entry.get("classification") == "deferred":
            errors.append(f"{name}: mandatory W04 acceptance is absent")
    return errors


def _load_inputs() -> tuple[dict[str, Any], dict[str, Any]]:
    return (
        json.loads(W01_MATRIX.read_text(encoding="utf-8")),
        json.loads(W03_PROFILE.read_text(encoding="utf-8")),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    args = parser.parse_args()

    try:
        matrix, w03 = _load_inputs()
        expected = build_profile(
            matrix,
            w03,
            matrix_sha256=_sha256(W01_MATRIX),
            w03_sha256=_sha256(W03_PROFILE),
        )
        errors = validate_profile(
            expected if args.write else json.loads(W04_PROFILE.read_text(encoding="utf-8")),
            matrix,
            w03,
            matrix_sha256=_sha256(W01_MATRIX),
            w03_sha256=_sha256(W03_PROFILE),
        )
        if errors:
            raise ValueError("; ".join(errors))
        if args.write:
            W04_PROFILE.parent.mkdir(parents=True, exist_ok=True)
            W04_PROFILE.write_text(
                json.dumps(expected, indent=2, sort_keys=False) + "\n",
                encoding="utf-8",
            )
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        print(f"W04 profile check failed: {error}", file=sys.stderr)
        return 1

    action = "generated" if args.write else "matches live W01/W03 inputs"
    print(
        f"W04 opcode profile {action}: "
        f"{expected['active_opcode_count']} active opcodes, no W04 deferrals"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
