#!/usr/bin/env python3
"""Generate the live, per-opcode W05 call-model profile."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
W01_MATRIX = ROOT / "docs/native-engine/semantics/opcodes/opcode-matrix.json"
W04_PROFILE = ROOT / "docs/native-engine/control-flow/w04-opcode-profile.json"
W05_PROFILE = ROOT / "docs/native-engine/calls/w05-opcode-profile.json"
RECLASSIFICATION = ROOT / "docs/native-engine/calls/w05-reclassification.json"

ACCEPTED = {
    "ZEND_INIT_FCALL",
    "ZEND_SEND_VAL",
    "ZEND_SEND_VAL_EX",
    "ZEND_SEND_VAR",
    "ZEND_SEND_VAR_EX",
    "ZEND_RECV",
    "ZEND_DO_UCALL",
    "ZEND_DO_FCALL",
}

LATER: dict[str, tuple[str, str]] = {
    "ZEND_SEND_VAR_NO_REF_EX": ("W06", "Reference/no-ref and COW semantics."),
    "ZEND_INIT_FCALL_BY_NAME": ("W10", "By-name target resolution."),
    "ZEND_INIT_NS_FCALL_BY_NAME": ("W10", "Namespace by-name target resolution."),
    "ZEND_INCLUDE_OR_EVAL": ("W10", "Dynamic declaration and include/eval semantics."),
    "ZEND_EXT_FCALL_BEGIN": ("W15", "Observer and extension ABI boundary."),
    "ZEND_EXT_FCALL_END": ("W15", "Observer and extension ABI boundary."),
    "ZEND_TICKS": ("W15", "Observer/runtime interoperability."),
    "ZEND_SEND_VAR_NO_REF": ("W06", "Reference/no-ref and COW semantics."),
    "ZEND_CATCH": ("W09", "Protected exception continuation."),
    "ZEND_THROW": ("W09", "Call exception propagation and cleanup."),
    "ZEND_INIT_METHOD_CALL": ("W08", "Object method target resolution."),
    "ZEND_INIT_STATIC_METHOD_CALL": ("W08", "Static method target resolution."),
    "ZEND_SEND_REF": ("W06", "By-reference argument ownership."),
    "ZEND_INIT_USER_CALL": ("W10", "Dynamic user-call target resolution."),
    "ZEND_SEND_ARRAY": ("W07", "Array argument containers."),
    "ZEND_SEND_USER": ("W10", "Dynamic user-call argument semantics."),
    "ZEND_INIT_DYNAMIC_CALL": ("W10", "Dynamic callable resolution."),
    "ZEND_DO_ICALL": ("W15", "Internal handler and C-ABI interoperability."),
    "ZEND_DO_FCALL_BY_NAME": ("W10", "By-name target resolution."),
    "ZEND_DECLARE_FUNCTION": ("W10", "Function declaration and target identity."),
    "ZEND_DECLARE_LAMBDA_FUNCTION": ("W10", "Lambda declaration and target identity."),
    "ZEND_DECLARE_CONST": ("W10", "Dynamic declaration ordering."),
    "ZEND_DECLARE_CLASS": ("W08", "Class and method target identity."),
    "ZEND_DECLARE_CLASS_DELAYED": ("W08", "Delayed class target identity."),
    "ZEND_DECLARE_ANON_CLASS": ("W08", "Anonymous class target identity."),
    "ZEND_HANDLE_EXCEPTION": ("W09", "Exception dispatch and cleanup."),
    "ZEND_ASSERT_CHECK": ("W09", "Assertion exception semantics."),
    "ZEND_CALL_TRAMPOLINE": ("W10", "Trampoline and callback resolution."),
    "ZEND_DISCARD_EXCEPTION": ("W09", "Protected exception cleanup."),
    "ZEND_YIELD": ("W11", "Generator suspension semantics."),
    "ZEND_FAST_CALL": ("W09", "Protected continuation control flow."),
    "ZEND_FAST_RET": ("W09", "Protected continuation control flow."),
    "ZEND_RECV_INIT": ("W07", "Default argument materialization."),
    "ZEND_RECV_VARIADIC": ("W07", "Variadic argument container."),
    "ZEND_SEND_UNPACK": ("W07", "Unpacked argument container."),
    "ZEND_YIELD_FROM": ("W11", "Generator delegation semantics."),
    "ZEND_SEND_FUNC_ARG": ("W06", "Target-dependent by-reference ownership."),
    "ZEND_GET_CALLED_CLASS": ("W08", "Late-bound class target resolution."),
    "ZEND_MATCH_ERROR": ("W09", "Exception creation and propagation."),
    "ZEND_VERIFY_NEVER_TYPE": ("W09", "Return exception propagation."),
    "ZEND_CALLABLE_CONVERT": ("W10", "Callable conversion and dynamic resolution."),
    "ZEND_FRAMELESS_ICALL_0": ("W15", "Frameless internal C-ABI call."),
    "ZEND_FRAMELESS_ICALL_1": ("W15", "Frameless internal C-ABI call."),
    "ZEND_FRAMELESS_ICALL_2": ("W15", "Frameless internal C-ABI call."),
    "ZEND_FRAMELESS_ICALL_3": ("W15", "Frameless internal C-ABI call."),
    "ZEND_JMP_FRAMELESS": ("W15", "Frameless internal C-ABI control transfer."),
    "ZEND_INIT_PARENT_PROPERTY_HOOK_CALL": ("W08", "Property-hook method target."),
    "ZEND_DECLARE_ATTRIBUTED_CONST": ("W10", "Attributed declaration ordering."),
    "ZEND_TYPE_ASSERT": ("W06", "Reference-aware runtime type proof."),
    "ZEND_CALLABLE_CONVERT_PARTIAL": ("W10", "Partial callable conversion."),
    "ZEND_SEND_PLACEHOLDER": ("W07", "Placeholder and named argument grammar."),
}

CALL_NAME_MARKERS = (
    "CALL", "SEND", "RECV", "FRAMELESS", "DECLARE", "EXCEPTION",
    "THROW", "CATCH", "YIELD", "ASSERT",
)

CALL_PROOFS = [
    "complete_reachable_call_scan",
    "atomic_init_send_do_plan",
    "exact_direct_same_script_user_target",
    "balanced_nested_call_stack",
    "positional_by_value_arguments",
    "exact_non_refcounted_scalar_arguments",
    "exact_argument_count",
    "no_defaults_named_unpack_variadic_placeholder",
    "no_by_reference",
    "result_unused_or_exact_scalar",
    "complete_caller_callee_frames",
    "no_protected_region",
]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def is_call_adjacent(entry: dict[str, Any]) -> bool:
    name = entry["opcode"]
    return name in ACCEPTED or name in LATER or entry.get("deferred_wave") == "W05" or any(
        marker in name for marker in CALL_NAME_MARKERS
    )


def build() -> tuple[dict[str, Any], dict[str, Any]]:
    matrix = json.loads(W01_MATRIX.read_text(encoding="utf-8"))
    w04 = json.loads(W04_PROFILE.read_text(encoding="utf-8"))
    w01_by_number = {entry["number"]: entry for entry in matrix["opcodes"]}
    entries = []
    reclassified = []
    adjacent_count = 0
    for old in w04["opcodes"]:
        source = w01_by_number.get(old["number"])
        if source is None or source["opcode"] != old["opcode"]:
            raise ValueError("W01/W04 opcode identity drift at %s" % old["number"])
        name = old["opcode"]
        adjacent = is_call_adjacent(old)
        if adjacent:
            adjacent_count += 1
            if name in ACCEPTED:
                classification = "conditional"
                deferred_wave = None
                proofs = CALL_PROOFS
                mir_opcodes = (
                    ["call_direct_user"]
                    if name in {"ZEND_DO_UCALL", "ZEND_DO_FCALL"} else []
                )
                rationale = (
                    "Accepted only as a fragment of a fully planned exact direct "
                    "same-script user-call sequence; never published independently."
                )
            elif name in LATER:
                deferred_wave, rationale = LATER[name]
                classification = "deferred"
                proofs = []
                mir_opcodes = []
            else:
                raise ValueError("unresolved call-adjacent opcode: %s" % name)
            if (
                classification != old["classification"]
                or deferred_wave != old.get("deferred_wave")
            ):
                reclassified.append({
                    "from_classification": old["classification"],
                    "from_wave": old.get("deferred_wave"),
                    "number": old["number"],
                    "opcode": name,
                    "rationale": rationale,
                    "to_classification": classification,
                    "to_wave": deferred_wave,
                })
        else:
            classification = "inherited" if old["classification"] != "deferred" else "deferred"
            deferred_wave = old.get("deferred_wave")
            proofs = old.get("proofs", [])
            mir_opcodes = old.get("mir_opcodes", [])
            rationale = "Inherited unchanged from the live W04 profile."
        entries.append({
            "classification": classification,
            "deferred_wave": deferred_wave,
            "mir_opcodes": mir_opcodes,
            "number": old["number"],
            "opcode": name,
            "proofs": proofs,
            "rationale": rationale,
            "source_refs": source["source_refs"],
            "w04_classification": old["classification"],
            "w04_deferred_wave": old.get("deferred_wave"),
        })
    unresolved = [
        entry["opcode"] for entry in entries
        if entry["deferred_wave"] == "W05"
    ]
    if unresolved:
        raise ValueError("unresolved W05 deferrals: %s" % ", ".join(unresolved))
    sources = {
        "w01": {
            "active_opcode_count": len(matrix["opcodes"]),
            "path": W01_MATRIX.relative_to(ROOT).as_posix(),
            "sha256": sha256(W01_MATRIX),
            "source_commit": matrix["source_commit"],
        },
        "w04": {
            "active_opcode_count": len(w04["opcodes"]),
            "path": W04_PROFILE.relative_to(ROOT).as_posix(),
            "sha256": sha256(W04_PROFILE),
        },
    }
    profile = {
        "active_opcode_count": len(entries),
        "call_adjacent_opcode_count": adjacent_count,
        "format_version": 1,
        "mir_contract_version": "1.7",
        "opcodes": entries,
        "proof_catalog": sorted(set(CALL_PROOFS)),
        "reserved_opcode_numbers": matrix["reserved_opcode_numbers"],
        "sources": sources,
        "unresolved_w05_count": 0,
    }
    changes = {
        "format_version": 1,
        "reclassifications": sorted(reclassified, key=lambda item: item["number"]),
        "sources": sources,
        "unresolved_w05_count": 0,
    }
    return profile, changes


def encoded(value: dict[str, Any]) -> str:
    return json.dumps(value, indent=2, sort_keys=True) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        profile, changes = build()
    except (OSError, json.JSONDecodeError, KeyError, ValueError) as exc:
        print("W05 profile generation failed: %s" % exc, file=sys.stderr)
        return 1
    outputs = ((W05_PROFILE, profile), (RECLASSIFICATION, changes))
    if args.check:
        stale = [
            path for path, value in outputs
            if not path.exists() or path.read_text(encoding="utf-8") != encoded(value)
        ]
        if stale:
            print("stale W05 profile outputs: %s" % ", ".join(str(path) for path in stale), file=sys.stderr)
            return 1
    else:
        for path, value in outputs:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(encoded(value), encoding="utf-8")
    print(
        "W05 profile valid: %d live opcodes, %d call-adjacent, zero unresolved"
        % (profile["active_opcode_count"], profile["call_adjacent_opcode_count"])
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
