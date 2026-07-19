#!/usr/bin/env python3
"""Validate the W05 implementation corpus deterministically."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
MANIFEST = Path(__file__).with_name("manifest.json")
REQUIRED_MODELED = {
    "direct_user_zero_args_unused_return",
    "direct_user_scalar_literals",
    "direct_user_scalar_result_bool",
    "direct_user_scalar_result_float",
    "direct_user_scalar_result_int",
    "direct_user_scalar_result_null",
    "direct_user_scalar_variables",
    "two_direct_calls_same_block",
    "direct_call_in_if",
    "direct_call_in_reducible_loop",
    "direct_call_after_phi",
    "direct_call_exact_user_do_fcall",
}
REQUIRED_DEFERRED = {
    "by_ref_argument": "W06",
    "refcounted_argument": "W06",
    "used_unknown_return": "W06",
    "named_argument": "W07",
    "named_argument_normalized_position": "W07",
    "unpack_argument": "W07",
    "variadic_target": "W07",
    "method_call": "W08",
    "static_method_call": "W08",
    "constructor_call": "W08",
    "try_catch_around_call": "W09",
    "dynamic_call": "W10",
    "by_name_call": "W10",
    "closure_callback": "W10",
    "recursive_self_call": "W10",
    "internal_function": "W15",
    "frameless_icall": "W15",
    "observer_specific_marker": "W15",
}


def validate() -> list[str]:
    errors: list[str] = []
    document = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if document.get("schema_version") != 1 or not isinstance(
        document.get("cases"), list
    ):
        return ["invalid manifest envelope"]
    cases = document["cases"]
    identifiers = [case.get("id") for case in cases]
    if len(identifiers) != len(set(identifiers)):
        errors.append("duplicate case id")
    by_id = {case.get("id"): case for case in cases}
    for identifier in sorted(REQUIRED_MODELED):
        case = by_id.get(identifier)
        if case is None:
            errors.append(f"missing modeled case {identifier}")
        elif (
            case.get("status") != "accepted"
            or case.get("mirl") != "MIRL0000"
            or case.get("wave") is not None
        ):
            errors.append(f"invalid modeled decision {identifier}")
    for identifier, wave in sorted(REQUIRED_DEFERRED.items()):
        case = by_id.get(identifier)
        if case is None:
            errors.append(f"missing deferred case {identifier}")
        elif case.get("status") != "rejected" or case.get("wave") != wave:
            errors.append(f"invalid deferred decision {identifier}")
    for case in cases:
        source = ROOT / str(case.get("source", ""))
        if not source.is_file():
            errors.append(f"missing source {case.get('source')}")
        if not re.fullmatch(r"MIRL[0-9]{4}", str(case.get("mirl", ""))):
            errors.append(f"invalid diagnostic {case.get('id')}")
        if not isinstance(case.get("function"), str) or not case["function"]:
            errors.append(f"invalid function {case.get('id')}")
    do_fcall = by_id.get("direct_call_exact_user_do_fcall", {})
    if do_fcall.get("compiler_mode") != "ignore_user_functions":
        errors.append("DO_FCALL proof must pin ignore_user_functions mode")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.parse_args()
    errors = validate()
    if errors:
        print("\n".join(errors))
        return 1
    print("W05 corpus manifest: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
