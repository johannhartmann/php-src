#!/usr/bin/env python3
"""Deterministic W06 bridge differential runner and self-test."""

from __future__ import annotations

import argparse
import base64
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CORPUS = ROOT / "tests/native/values/corpus/manifest.json"


def canonical_manifest() -> bytes:
    data = json.loads(CORPUS.read_text(encoding="utf-8"))
    return json.dumps(data, sort_keys=True, separators=(",", ":")).encode()


def php_array(options: dict[str, object]) -> str:
    fields = []
    for key, value in options.items():
        fields.append(f"{json.dumps(key)}=>{json.dumps(value)}")
    return "[" + ",".join(fields) + "]"


def by_ref_parameter_source(parameter: int) -> str:
    if parameter < 1 or parameter > 4096:
        raise ValueError("by-ref parameter generator is out of bounds")
    parameters = [f"int $p{index}" for index in range(1, parameter + 1)]
    target_parameters = parameters.copy()
    target_parameters[parameter - 1] = (
        "int &$p" + str(parameter)
    )
    arguments = [f"$p{index}" for index in range(1, parameter + 1)]
    return (
        "<?php function g("
        + ",".join(target_parameters)
        + f"): int{{return $p{parameter};}} function f("
        + ",".join(parameters)
        + "): int{return g("
        + ",".join(arguments)
        + ");}"
    )


def case_source(case: dict[str, object]) -> str:
    source = case.get("source")
    if isinstance(source, str):
        return source
    if case.get("generator") == "by_ref_parameter":
        parameter = case.get("parameter")
        if isinstance(parameter, int):
            return by_ref_parameter_source(parameter)
    raise RuntimeError(f"{case.get('name')}: unsupported source generator")


def run_bridge(
    php: Path,
    source: str,
    *,
    filename: str = "w06.php",
    function: str | None = None,
    fault: str | None = None,
) -> dict[str, object]:
    options: dict[str, object] = {"wave": 6}
    if function is not None:
        options["function"] = function
    if fault is not None:
        options["fault"] = fault
    encoded = base64.b64encode(source.encode()).decode("ascii")
    program = (
        "$r=native_mir_test_compile_dump(base64_decode("
        + json.dumps(encoded)
        + ",true),"
        + json.dumps(filename)
        + ","
        + php_array(options)
        + ");"
        "echo json_encode($r,JSON_UNESCAPED_SLASHES);"
    )
    completed = subprocess.run(
        [str(php), "-n", "-r", program],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"candidate PHP exited {completed.returncode}: {completed.stderr.strip()}"
        )
    try:
        result = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError(
            f"candidate PHP returned non-JSON output: {completed.stdout!r}"
        ) from error
    if not isinstance(result, dict):
        raise RuntimeError("W06 bridge result is not an object")
    return result


def verify_case(php: Path, case: dict[str, object]) -> None:
    result = run_bridge(
        php,
        case_source(case),
        filename=f"{case['name']}.php",
        function=case.get("function") if isinstance(case.get("function"), str) else None,
    )
    if result.get("status") != case["expected_status"]:
        raise RuntimeError(
            f"{case['name']}: status {result.get('status')!r}, "
            f"expected {case['expected_status']!r}"
        )
    diagnostics = result.get("diagnostics")
    codes = {
        item.get("code")
        for item in diagnostics
        if isinstance(item, dict)
    } if isinstance(diagnostics, list) else set()
    expected_code = case.get("expected_code")
    if expected_code is not None and expected_code not in codes:
        raise RuntimeError(
            f"{case['name']}: missing diagnostic {expected_code!r}; got {codes}"
        )
    source_opcodes = result.get("source_opcodes")
    if not isinstance(source_opcodes, list):
        raise RuntimeError(f"{case['name']}: missing source opcode inventory")
    for opcode in case.get("required_source_opcodes", []):
        if opcode not in source_opcodes:
            raise RuntimeError(f"{case['name']}: missing source opcode {opcode}")
    mir = result.get("mir")
    if case["expected_status"] == "accepted":
        if not isinstance(mir, str):
            raise RuntimeError(f"{case['name']}: accepted result has no MIR")
        for token in case.get("required_mir_tokens", []):
            if token not in mir:
                raise RuntimeError(f"{case['name']}: missing MIR token {token!r}")
    elif mir is not None:
        raise RuntimeError(f"{case['name']}: non-accepted result published MIR")


FAULTS = (
    "compile_bailout",
    "finalize_failure",
    "stage1_verifier_failure",
    "stage2_verifier_failure",
    "structural_verifier_failure",
    "scalar_verifier_failure",
    "control_flow_verifier_failure",
    "call_verifier_failure",
    "fingerprint_recompute_failure",
    "value_inventory",
    "value_plan",
    "value_storage",
    "value_reference",
    "value_alias",
    "value_event",
    "value_separation",
    "value_call_transfer",
    "value_verifier_failure",
    "dump_failure",
)


def verify_faults(php: Path, source: str, function: str) -> None:
    for fault in FAULTS:
        result = run_bridge(
            php, source, filename=f"fault-{fault}.php",
            function=function, fault=fault,
        )
        if result.get("status") == "accepted" or result.get("mir") is not None:
            raise RuntimeError(f"{fault}: fault did not fail atomically")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--candidate-php", type=Path)
    args = parser.parse_args()
    first = canonical_manifest()
    second = canonical_manifest()
    if first != second or b'"by_ref_parameter_128"' not in first:
        raise SystemExit("W06 differential self-test failed")
    data = json.loads(first)
    if args.candidate_php:
        candidate = args.candidate_php.resolve()
        if not candidate.is_file():
            raise SystemExit(f"candidate PHP does not exist: {candidate}")
        for section in ("modeled", "deferred"):
            for case in data[section]:
                if case.get("runtime", True):
                    verify_case(candidate, case)
        fault_case = next(
            case for case in data["modeled"]
            if case["name"] == "direct_user_by_ref_argument"
        )
        verify_faults(
            candidate, case_source(fault_case), str(fault_case["function"])
        )
    print("W06 differential self-test: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
