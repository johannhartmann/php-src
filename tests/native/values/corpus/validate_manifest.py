#!/usr/bin/env python3
"""Validate the deterministic W06 A2 corpus inventory."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
MANIFEST = ROOT / "manifest.json"
PROFILE = ROOT.parents[3] / "docs/native-engine/values/w06-opcode-profile.json"
EXPECTED_MODELED = {
    "local_make_ref",
    "local_assign_ref",
    "local_reference_rebinding",
    "local_alias_write_scalar",
    "local_unset_and_isset",
    "copy_tmp_refcounted_string",
    "separate_shared_string_protocol",
    "direct_user_by_ref_argument",
    "direct_user_refcounted_string_argument",
    "direct_user_refcounted_string_return",
    "direct_user_by_ref_return",
    "nested_direct_refcounted_result",
    "by_ref_parameter_65",
    "by_ref_parameter_128",
    "exact_recursive_direct_call_with_reference_model",
}
EXPECTED_DEFERRED = {
    "array_write_or_clone": "W07",
    "string_concat_or_rope": "W07",
    "object_reference_or_property": "W08",
    "destructor_observable_overwrite": "W09",
    "exception_during_release": "W09",
    "global_or_lexical_binding": "W10",
    "dynamic_symbol_table_alias": "W10",
    "internal_function_reference_transfer": "W15",
    "echo_or_silence_runtime": "W15",
    "unrelated_scalar_gap": "W16",
}


def validate() -> dict[str, object]:
    data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if data.get("format_version") != 2:
        raise SystemExit("invalid W06 corpus format")
    modeled = data.get("modeled", [])
    deferred = data.get("deferred", [])
    names = [entry.get("name") for entry in modeled]
    if len(names) != len(set(names)) or set(names) != EXPECTED_MODELED:
        raise SystemExit("modeled W06 corpus drift")
    decisions = {entry.get("name"): entry.get("later_wave") for entry in deferred}
    if decisions != EXPECTED_DEFERRED:
        raise SystemExit("deferred W06 corpus drift")
    parameters = {entry.get("parameter") for entry in modeled if "parameter" in entry}
    if parameters != {65, 128}:
        raise SystemExit("scalable parameter corpus is incomplete")
    profile = json.loads(PROFILE.read_text(encoding="utf-8"))
    accepted = {
        entry["opcode"]
        for entry in profile["opcodes"]
        if entry["decision"] == "accepted"
    }
    for entry in modeled:
        if not isinstance(entry.get("runtime"), bool):
            raise SystemExit(f"{entry.get('name')}: missing runtime decision")
        if entry["runtime"]:
            if entry.get("expected_status") != "accepted":
                raise SystemExit(f"{entry['name']}: modeled runtime case is not accepted")
            if not isinstance(entry.get("function"), str):
                raise SystemExit(f"{entry['name']}: missing selected function")
            if not isinstance(entry.get("source"), str) and entry.get("generator") != "by_ref_parameter":
                raise SystemExit(f"{entry['name']}: missing source or supported generator")
            if not entry.get("required_source_opcodes"):
                raise SystemExit(f"{entry['name']}: missing source opcode evidence")
            if not entry.get("required_mir_tokens"):
                raise SystemExit(f"{entry['name']}: missing MIR property evidence")
            exact_counts = entry.get("exact_mir_token_counts", {})
            if not isinstance(exact_counts, dict) or any(
                not isinstance(token, str)
                or not token
                or not isinstance(expected, int)
                or isinstance(expected, bool)
                or expected < 0
                for token, expected in exact_counts.items()
            ):
                raise SystemExit(
                    f"{entry['name']}: invalid exact MIR token counts"
                )
        else:
            if entry.get("profile_opcode") not in accepted:
                raise SystemExit(
                    f"{entry['name']}: static evidence is not a live accepted opcode"
                )
            if not isinstance(entry.get("evidence"), str):
                raise SystemExit(f"{entry['name']}: missing static evidence description")
    for entry in deferred:
        if entry.get("runtime") is not True:
            raise SystemExit(f"{entry.get('name')}: deferred case must run")
        if entry.get("expected_status") not in {"rejected", "deferred"}:
            raise SystemExit(f"{entry['name']}: deferred case has permissive status")
        if not isinstance(entry.get("expected_code"), str):
            raise SystemExit(f"{entry['name']}: missing exact diagnostic")
        if not isinstance(entry.get("source"), str) or not isinstance(
            entry.get("function"), str
        ):
            raise SystemExit(f"{entry['name']}: missing executable source")
        if not entry.get("required_source_opcodes"):
            raise SystemExit(f"{entry['name']}: missing source opcode evidence")
    return data


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.parse_args()
    data = validate()
    print(
        f"W06 corpus valid: modeled={len(data['modeled'])} "
        f"deferred={len(data['deferred'])}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
