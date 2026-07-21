#!/usr/bin/env python3
"""Generate the live, per-opcode W06 value/reference profile."""

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
W06_PROFILE = ROOT / "docs/native-engine/values/w06-opcode-profile.json"
RECLASSIFICATION = ROOT / "docs/native-engine/values/w06-reclassification.json"

PROOFS = {
    "complete_value_inventory",
    "atomic_value_plan",
    "local_storage_only",
    "old_value_transition_complete",
    "stable_reference_cell_identity",
    "reference_lvalue_exact",
    "abstract_refcount_state",
    "cleanup_debt_explicit",
    "alias_merge_conservative",
    "separation_requirement_complete",
    "exact_direct_user_call",
    "scalable_parameter_modes",
    "call_transfer_complete",
    "same_module_fingerprint",
}

MODELED_SEMANTICS = [
    "zval_storage_model",
    "reference_cell_model",
    "indirect_slot_model",
    "refcount_transfer_model",
    "alias_partition_model",
    "separation_protocol_model",
    "direct_user_call_reference_transfer_model",
    "refcounted_call_result_model",
]

EXECUTION_BOUNDARIES = [
    "container_clone_execution",
    "string_and_array_operation_semantics",
    "object_lifecycle",
    "destructor_exception_cleanup",
    "runtime_reference_binding",
    "dynamic_symbol_table_aliasing",
    "call_execution",
    "internal_c_abi_interop",
]

ACCEPTED: dict[str, tuple[str, list[str], str]] = {
    "ZEND_ASSIGN": ("storage_write", ["complete_value_inventory", "atomic_value_plan", "local_storage_only", "old_value_transition_complete"], "Local storage assignment with a complete old-value transition."),
    "ZEND_ASSIGN_REF": ("reference_bind", ["complete_value_inventory", "atomic_value_plan", "local_storage_only", "stable_reference_cell_identity", "reference_lvalue_exact"], "Local binding to an exact stable reference cell."),
    "ZEND_CHECK_VAR": ("storage_observe", ["complete_value_inventory", "local_storage_only"], "Local undef/direct/reference observation."),
    "ZEND_SEND_VAR_NO_REF_EX": ("call_argument_transfer", ["complete_value_inventory", "atomic_value_plan", "exact_direct_user_call", "scalable_parameter_modes", "call_transfer_complete"], "Exact direct-user-call no-ref argument transfer."),
    "ZEND_SEND_REF": ("call_argument_transfer", ["complete_value_inventory", "atomic_value_plan", "exact_direct_user_call", "reference_lvalue_exact", "call_transfer_complete"], "Exact direct-user-call by-reference transfer."),
    "ZEND_CHECK_FUNC_ARG": ("call_parameter_mode", ["complete_value_inventory", "exact_direct_user_call", "scalable_parameter_modes"], "Exact scalable parameter-mode lookup."),
    "ZEND_SEND_VAR_NO_REF": ("call_argument_transfer", ["complete_value_inventory", "atomic_value_plan", "exact_direct_user_call", "scalable_parameter_modes", "call_transfer_complete"], "Exact direct-user-call no-ref argument transfer."),
    "ZEND_RETURN_BY_REF": ("call_return_reference", ["complete_value_inventory", "atomic_value_plan", "stable_reference_cell_identity", "reference_lvalue_exact"], "Exact variable reference return, modeled without runtime binding."),
    "ZEND_MAKE_REF": ("reference_create", ["complete_value_inventory", "atomic_value_plan", "local_storage_only", "stable_reference_cell_identity"], "Creates a stable abstract local reference cell."),
    "ZEND_UNSET_CV": ("storage_release", ["complete_value_inventory", "atomic_value_plan", "local_storage_only", "abstract_refcount_state", "cleanup_debt_explicit"], "Local storage release with explicit cleanup debt."),
    "ZEND_ISSET_ISEMPTY_CV": ("storage_observe", ["complete_value_inventory", "local_storage_only"], "Local undef/direct/reference observation."),
    "ZEND_SEPARATE": ("separation_protocol", ["complete_value_inventory", "atomic_value_plan", "abstract_refcount_state", "alias_merge_conservative", "separation_requirement_complete"], "Abstract separation requirement only; no clone execution."),
    "ZEND_COPY_TMP": ("refcount_transfer", ["complete_value_inventory", "atomic_value_plan", "abstract_refcount_state", "cleanup_debt_explicit"], "Explicit copy-addref or move transition."),
    "ZEND_SEND_FUNC_ARG": ("call_argument_transfer", ["complete_value_inventory", "atomic_value_plan", "exact_direct_user_call", "scalable_parameter_modes", "reference_lvalue_exact", "call_transfer_complete"], "Target-proven exact direct-user-call argument transfer."),
}

VARIANTS: dict[str, tuple[str, list[str], str]] = {
    name: (
        "call_sequence_refcount_variant",
        ["complete_value_inventory", "atomic_value_plan", "exact_direct_user_call", "abstract_refcount_state", "scalable_parameter_modes", "call_transfer_complete", "same_module_fingerprint"],
        "W05 call fragment retained with an additional proof-closed refcount/reference transfer variant.",
    )
    for name in (
        "ZEND_INIT_FCALL", "ZEND_SEND_VAL", "ZEND_SEND_VAL_EX", "ZEND_SEND_VAR",
        "ZEND_SEND_VAR_EX", "ZEND_RECV", "ZEND_DO_UCALL", "ZEND_DO_FCALL",
    )
}
VARIANTS.update({
    "ZEND_QM_ASSIGN": (
        "refcount_copy_variant",
        ["complete_value_inventory", "atomic_value_plan", "abstract_refcount_state", "cleanup_debt_explicit"],
        "Existing scalar lowering retained with an explicit refcounted copy/move variant.",
    ),
    "ZEND_FREE": (
        "refcount_release_variant",
        ["complete_value_inventory", "atomic_value_plan", "abstract_refcount_state", "cleanup_debt_explicit"],
        "Existing scalar lowering retained with an explicit refcounted release variant.",
    ),
    "ZEND_RETURN": (
        "refcount_return_variant",
        ["complete_value_inventory", "atomic_value_plan", "abstract_refcount_state", "call_transfer_complete", "same_module_fingerprint"],
        "Existing scalar return retained with an explicit refcounted result-transfer variant.",
    ),
})

LATER: dict[str, tuple[str, str]] = {
    "ZEND_CONCAT": ("W07", "Concrete string concatenation and allocation."),
    "ZEND_ASSIGN_DIM_OP": ("W07", "Array/dimension container update."),
    "ZEND_FAST_CONCAT": ("W07", "Concrete string concatenation."),
    "ZEND_ROPE_INIT": ("W07", "Concrete rope/string container construction."),
    "ZEND_ROPE_ADD": ("W07", "Concrete rope/string container construction."),
    "ZEND_ROPE_END": ("W07", "Concrete rope/string container construction."),
    "ZEND_ADD_ARRAY_ELEMENT": ("W07", "Array container materialization."),
    "ZEND_UNSET_DIM": ("W07", "Array/dimension container mutation."),
    "ZEND_FETCH_DIM_R": ("W07", "Array/dimension lookup semantics."),
    "ZEND_FETCH_DIM_IS": ("W07", "Array/dimension isset lookup semantics."),
    "ZEND_FETCH_DIM_FUNC_ARG": ("W07", "Array/dimension call-argument lookup."),
    "ZEND_FETCH_LIST_R": ("W07", "List/array container extraction."),
    "ZEND_STRLEN": ("W07", "Concrete string operation semantics."),
    "ZEND_FE_FETCH_RW": ("W07", "Writable iteration and container aliasing."),
    "ZEND_FE_FREE": ("W07", "Iterator/container cleanup."),
    "ZEND_FETCH_LIST_W": ("W07", "List/array container write."),
    "ZEND_COALESCE": ("W07", "Container/string-aware coalescing semantics."),
    "ZEND_IN_ARRAY": ("W07", "Concrete array operation semantics."),
    "ZEND_COUNT": ("W07", "Concrete array/countable operation semantics."),
    "ZEND_ASSIGN_OBJ": ("W08", "Object property storage and lifecycle."),
    "ZEND_ASSIGN_OBJ_OP": ("W08", "Object property update and lifecycle."),
    "ZEND_ASSIGN_STATIC_PROP_OP": ("W08", "Static property storage and lifecycle."),
    "ZEND_ASSIGN_OBJ_REF": ("W08", "Object property reference binding."),
    "ZEND_ASSIGN_STATIC_PROP_REF": ("W08", "Static property reference binding."),
    "ZEND_FETCH_OBJ_R": ("W08", "Object property lookup."),
    "ZEND_FETCH_OBJ_IS": ("W08", "Object property isset lookup."),
    "ZEND_FETCH_OBJ_FUNC_ARG": ("W08", "Object property call-argument lookup."),
    "ZEND_FETCH_CLASS": ("W08", "Class identity resolution."),
    "ZEND_INSTANCEOF": ("W08", "Class/object semantics."),
    "ZEND_FETCH_CLASS_NAME": ("W08", "Class identity and name semantics."),
    "ZEND_FETCH_CLASS_CONSTANT": ("W08", "Class constant resolution."),
    "ZEND_FETCH_THIS": ("W08", "Object identity and frame semantics."),
    "ZEND_ISSET_ISEMPTY_THIS": ("W08", "Object identity observation."),
    "ZEND_GET_CLASS": ("W08", "Class/object semantics."),
    "ZEND_VERIFY_RETURN_TYPE": ("W09", "Observable type-error and cleanup path."),
    "ZEND_TYPE_ASSERT": ("W09", "Observable type-error and cleanup path."),
    "ZEND_UNSET_VAR": ("W10", "Dynamic variable and symbol-table aliasing."),
    "ZEND_FETCH_R": ("W10", "General variable fetch may use dynamic storage."),
    "ZEND_FETCH_W": ("W10", "General writable variable fetch may use dynamic storage."),
    "ZEND_FETCH_RW": ("W10", "General read/write variable fetch may use dynamic storage."),
    "ZEND_FETCH_IS": ("W10", "General isset fetch may use dynamic storage."),
    "ZEND_FETCH_FUNC_ARG": ("W10", "General call-argument fetch may use dynamic storage."),
    "ZEND_FETCH_UNSET": ("W10", "General unset fetch may use dynamic storage."),
    "ZEND_FETCH_CONSTANT": ("W10", "Dynamic constant resolution."),
    "ZEND_ISSET_ISEMPTY_VAR": ("W10", "Dynamic variable and symbol-table lookup."),
    "ZEND_DEFINED": ("W10", "Dynamic constant resolution."),
    "ZEND_BIND_GLOBAL": ("W10", "Global symbol-table reference binding."),
    "ZEND_FUNC_NUM_ARGS": ("W10", "Runtime frame argument introspection."),
    "ZEND_FUNC_GET_ARGS": ("W10", "Runtime frame argument container materialization."),
    "ZEND_BIND_LEXICAL": ("W10", "Lexical binding and closure storage."),
    "ZEND_BIND_STATIC": ("W10", "Static variable binding."),
    "ZEND_CHECK_UNDEF_ARGS": ("W10", "Runtime frame argument state."),
    "ZEND_FETCH_GLOBALS": ("W10", "Global symbol-table aliasing."),
    "ZEND_BIND_INIT_STATIC_OR_JMP": ("W10", "Static variable binding and initialization."),
    "ZEND_BEGIN_SILENCE": ("W15", "Runtime error-reporting boundary."),
    "ZEND_END_SILENCE": ("W15", "Runtime error-reporting boundary."),
    "ZEND_EXT_STMT": ("W15", "Observer/extension runtime boundary."),
    "ZEND_EXT_NOP": ("W15", "Observer/extension runtime boundary."),
    "ZEND_ECHO": ("W15", "Observable runtime output boundary."),
    "ZEND_DIV": ("W16", "Unrelated scalar arithmetic gap."),
    "ZEND_POW": ("W16", "Unrelated scalar arithmetic gap."),
    "ZEND_ASSIGN_OP": ("W16", "Generic compound-operation semantics."),
    "ZEND_PRE_INC": ("W16", "Generic increment semantics."),
    "ZEND_PRE_DEC": ("W16", "Generic decrement semantics."),
    "ZEND_POST_INC": ("W16", "Generic increment semantics."),
    "ZEND_POST_DEC": ("W16", "Generic decrement semantics."),
    "ZEND_CASE": ("W16", "Unrelated comparison semantics."),
    "ZEND_TYPE_CHECK": ("W16", "Unrelated runtime type-check semantics."),
    "ZEND_JMP_SET": ("W16", "Unrelated value-producing control-flow semantics."),
    "ZEND_GET_TYPE": ("W16", "Unrelated runtime type query."),
    "ZEND_CASE_STRICT": ("W16", "Unrelated comparison semantics."),
    "ZEND_JMP_NULL": ("W16", "Unrelated value-producing control-flow semantics."),
}

OWNER_SEQUENCE = {
    "ZEND_OP_DATA": "Owned by its immediately preceding opcode sequence; never lowered independently."
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build() -> tuple[dict[str, Any], dict[str, Any]]:
    matrix = json.loads(W01_MATRIX.read_text(encoding="utf-8"))
    w04 = json.loads(W04_PROFILE.read_text(encoding="utf-8"))
    w05 = json.loads(W05_PROFILE.read_text(encoding="utf-8"))
    w01_by_number = {entry["number"]: entry for entry in matrix["opcodes"]}
    w04_by_number = {entry["number"]: entry for entry in w04["opcodes"]}
    entries: list[dict[str, Any]] = []
    changes: list[dict[str, Any]] = []
    live_w06 = {
        entry["opcode"] for entry in w05["opcodes"]
        if entry.get("deferred_wave") == "W06"
    }
    decided_w06 = set(ACCEPTED) | set(LATER) | set(OWNER_SEQUENCE)
    if live_w06 != decided_w06:
        missing = sorted(live_w06 - decided_w06)
        stale = sorted(decided_w06 - live_w06)
        raise ValueError(f"W06 decision inventory drift (missing={missing}, stale={stale})")
    for old in w05["opcodes"]:
        source = w01_by_number.get(old["number"])
        control = w04_by_number.get(old["number"])
        if source is None or source["opcode"] != old["opcode"]:
            raise ValueError(f"W01/W05 opcode identity drift at {old['number']}")
        if control is None or control["opcode"] != old["opcode"]:
            raise ValueError(f"W04/W05 opcode identity drift at {old['number']}")
        name = old["opcode"]
        if name in ACCEPTED:
            transition, proofs, rationale = ACCEPTED[name]
            decision, later_wave = "accepted", None
        elif name in VARIANTS:
            transition, proofs, rationale = VARIANTS[name]
            decision, later_wave = "accepted_variant", None
        elif name in LATER:
            later_wave, rationale = LATER[name]
            decision, transition, proofs = "deferred", "none", []
        elif name in OWNER_SEQUENCE:
            decision, later_wave, transition, proofs = "owner_sequence", None, "owner_sequence", []
            rationale = OWNER_SEQUENCE[name]
        else:
            decision = "inherited" if old["classification"] != "deferred" else "deferred"
            later_wave = old.get("deferred_wave")
            transition = "inherited"
            proofs = old.get("proofs", [])
            rationale = "Inherited unchanged from the live W05-v2 profile."
        entry = {
            "decision": decision,
            "later_wave": later_wave,
            "number": old["number"],
            "opcode": name,
            "proofs": proofs,
            "rationale": rationale,
            "source_refs": source["source_refs"],
            "transition_family": transition,
            "w05_classification": old["classification"],
            "w05_deferred_wave": old.get("deferred_wave"),
        }
        entries.append(entry)
        if old.get("deferred_wave") == "W06" or name in VARIANTS:
            changes.append({
                "from_classification": old["classification"],
                "from_wave": old.get("deferred_wave"),
                "number": old["number"],
                "opcode": name,
                "rationale": rationale,
                "to_decision": decision,
                "to_wave": later_wave,
                "transition_family": transition,
            })
    unresolved = [entry["opcode"] for entry in entries if entry["later_wave"] == "W06"]
    if unresolved:
        raise ValueError("unresolved W06 deferrals: " + ", ".join(unresolved))
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
        "w05_v2": {
            "active_opcode_count": len(w05["opcodes"]),
            "path": W05_PROFILE.relative_to(ROOT).as_posix(),
            "sha256": sha256(W05_PROFILE),
        },
    }
    profile = {
        "active_opcode_count": len(entries),
        "accepted_variant_count": sum(entry["decision"] == "accepted_variant" for entry in entries),
        "codegen_eligible": False,
        "execution_boundaries": EXECUTION_BOUNDARIES,
        "format_version": 1,
        "live_w06_deferred_count": len(live_w06),
        "mir_contract_version": "1.9",
        "opcodes": entries,
        "proof_catalog": sorted(PROOFS),
        "reserved_opcode_numbers": matrix["reserved_opcode_numbers"],
        "modeled_semantics": MODELED_SEMANTICS,
        "sources": sources,
        "unresolved_w06_count": 0,
        "w06_accepted_count": sum(entry["decision"] == "accepted" for entry in entries),
    }
    reclassification = {
        "format_version": 1,
        "reclassifications": sorted(changes, key=lambda item: item["number"]),
        "sources": sources,
        "unresolved_w06_count": 0,
    }
    return profile, reclassification


def encoded(value: dict[str, Any]) -> str:
    return json.dumps(value, indent=2, sort_keys=True) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        profile, changes = build()
    except (OSError, json.JSONDecodeError, KeyError, ValueError) as exc:
        print(f"W06 profile generation failed: {exc}", file=sys.stderr)
        return 1
    outputs = ((W06_PROFILE, profile), (RECLASSIFICATION, changes))
    if args.check:
        stale = [
            path for path, value in outputs
            if not path.exists() or path.read_text(encoding="utf-8") != encoded(value)
        ]
        if stale:
            print("stale W06 profile outputs: " + ", ".join(str(path) for path in stale), file=sys.stderr)
            return 1
    else:
        for path, value in outputs:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(encoded(value), encoding="utf-8")
    print(
        "W06 profile valid: %d live opcodes, %d W06 decisions, %d variants, zero unresolved"
        % (
            profile["active_opcode_count"],
            profile["live_w06_deferred_count"],
            profile["accepted_variant_count"],
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
