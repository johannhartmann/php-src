#!/usr/bin/env python3
"""Execute the accepted W03/W04 scalar slice as native machine code."""

from __future__ import annotations

import argparse
import base64
import json
import os
import platform
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
TARGETS = {
    "darwin-arm64-dev": ("Darwin", "arm64", "arm64-apple-darwin", "ret"),
    "linux-amd64-prod": ("Linux", "x86_64", "x86_64-unknown-linux-gnu", "ret"),
}


@dataclass(frozen=True)
class Case:
    source: Path
    function: str
    wave: int
    inputs: tuple[tuple[Any, ...], ...]


CASES = (
    Case(Path("tests/native/lowering/corpus/accepted/arithmetic.php"),
         "w03_arithmetic", 3, ((17,), (-9,))),
    Case(Path("tests/native/lowering/corpus/accepted/bitwise.php"),
         "w03_bitwise", 3, ((12, 10), (-1, 5))),
    Case(Path("tests/native/lowering/corpus/accepted/booleans.php"),
         "w03_booleans", 3, ((False, True), (True, False))),
    Case(Path("tests/native/lowering/corpus/accepted/casts.php"),
         "w03_casts", 3, ((True,), (False,))),
    Case(Path("tests/native/lowering/corpus/accepted/comparisons.php"),
         "w03_comparisons", 3, ((4, 9), (9, 4))),
    Case(Path("tests/native/lowering/corpus/accepted/constants.php"),
         "w03_constants", 3, ((16,), (-1,))),
    Case(Path("tests/native/lowering/corpus/accepted/copy_move_return.php"),
         "w03_copy_move_return", 3, ((17,), (-8,))),
    Case(Path("tests/native/tpde/abi/double.php"),
         "w06_double", 3, ((1.5, 4), (-1.0, -2))),
    Case(Path("tests/native/tpde/abi/mixed.php"),
         "w06_mixed", 3, ((4, 0.5, True), (-2, 0.25, False))),
    Case(Path("tests/native/control-flow/corpus/accepted/if_else_int.php"),
         "w04_if_else_int", 4, ((False,), (True,))),
    Case(Path("tests/native/control-flow/corpus/accepted/nested_if_bool.php"),
         "w04_nested_if_bool", 4,
         ((False, False), (True, False), (True, True))),
    Case(Path("tests/native/control-flow/corpus/accepted/ternary_scalar.php"),
         "w04_ternary_scalar", 4, ((True, 7, 9), (False, 7, 9))),
    Case(Path("tests/native/control-flow/corpus/accepted/short_circuit_and.php"),
         "w04_short_circuit_and", 4,
         ((False, True), (True, False), (True, True))),
    Case(Path("tests/native/control-flow/corpus/accepted/short_circuit_or.php"),
         "w04_short_circuit_or", 4,
         ((False, False), (False, True), (True, False))),
    Case(Path("tests/native/control-flow/corpus/accepted/early_return.php"),
         "w04_early_return", 4, ((True, 4), (False, 4))),
    Case(Path("tests/native/control-flow/corpus/accepted/empty_fallthrough.php"),
         "w04_empty_fallthrough", 4, ((False, 5), (True, 6))),
    Case(Path("tests/native/control-flow/corpus/accepted/while_loop_counter.php"),
         "w04_while_loop_counter", 4, ((False,),)),
    Case(Path("tests/native/control-flow/corpus/accepted/do_while_counter.php"),
         "w04_do_while_counter", 4, ((False,),)),
    Case(Path("tests/native/control-flow/corpus/accepted/for_loop_counter.php"),
         "w04_for_loop_counter", 4, ((False,),)),
    Case(Path("tests/native/control-flow/corpus/accepted/loop_carried_phi.php"),
         "w04_loop_carried_phi", 4, ((False, False, False),)),
    Case(
        Path(
            "tests/native/control-flow/corpus/accepted/"
            "multiple_backedges_reducible.php"
        ),
        "w04_multiple_backedges_reducible",
        4,
        ((False, False),),
    ),
)


REFERENCE_RUNNER = r"""
$source = base64_decode(getenv("NATIVE_SOURCE"), true);
$arguments = json_decode(getenv("NATIVE_ARGUMENTS"), true, 32, JSON_THROW_ON_ERROR);
$function = getenv("NATIVE_FUNCTION");
if ($source === false || !str_starts_with($source, "<?php")) {
    throw new RuntimeException("invalid source");
}
$source = substr($source, 5);
ob_start();
try {
    eval($source);
} finally {
    ob_end_clean();
}
echo json_encode($function(...$arguments), JSON_PRESERVE_ZERO_FRACTION | JSON_THROW_ON_ERROR);
"""

CANDIDATE_RUNNER = r"""
$source = base64_decode(getenv("NATIVE_SOURCE"), true);
$arguments = json_decode(getenv("NATIVE_ARGUMENTS"), true, 32, JSON_THROW_ON_ERROR);
$options = [
    "wave" => (int) getenv("NATIVE_WAVE"),
    "function" => getenv("NATIVE_FUNCTION"),
    "target" => getenv("NATIVE_TARGET"),
    "repeat" => (int) getenv("NATIVE_REPEAT"),
];
$fault = getenv("NATIVE_FAULT");
if ($fault !== "") {
    $options["fault"] = $fault;
}
echo json_encode(
    native_mir_test_compile_execute(
        $source, getenv("NATIVE_FILENAME"), $arguments, $options
    ),
    JSON_PRESERVE_ZERO_FRACTION | JSON_THROW_ON_ERROR
);
"""

RESET_RUNNER = r"""
$firstSource = base64_decode(getenv("NATIVE_SOURCE"), true);
$secondSource = base64_decode(getenv("NATIVE_SOURCE_TWO"), true);
$firstArguments = json_decode(getenv("NATIVE_ARGUMENTS"), true, 32, JSON_THROW_ON_ERROR);
$secondArguments = json_decode(getenv("NATIVE_ARGUMENTS_TWO"), true, 32, JSON_THROW_ON_ERROR);
$target = getenv("NATIVE_TARGET");
$first = native_mir_test_compile_execute(
    $firstSource,
    getenv("NATIVE_FILENAME"),
    $firstArguments,
    ["wave" => 3, "function" => getenv("NATIVE_FUNCTION"), "target" => $target]
);
$second = native_mir_test_compile_execute(
    $secondSource,
    getenv("NATIVE_FILENAME_TWO"),
    $secondArguments,
    ["wave" => 4, "function" => getenv("NATIVE_FUNCTION_TWO"), "target" => $target]
);
echo json_encode([$first, $second], JSON_PRESERVE_ZERO_FRACTION | JSON_THROW_ON_ERROR);
"""


def run_php(binary: Path, program: str, env: dict[str, str]) -> Any:
    completed = subprocess.run(
        [str(binary), "-n", "-r", program],
        cwd=ROOT,
        env={**os.environ, **env},
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"{binary} exited {completed.returncode}: {completed.stderr.strip()}"
        )
    if completed.stderr:
        raise RuntimeError(f"{binary} wrote to stderr: {completed.stderr.strip()}")
    try:
        return json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError(
            f"{binary} returned non-JSON output: {completed.stdout!r}"
        ) from error


def environment(case: Case, arguments: tuple[Any, ...], target: str) -> dict[str, str]:
    source = (ROOT / case.source).read_bytes()
    return {
        "NATIVE_SOURCE": base64.b64encode(source).decode("ascii"),
        "NATIVE_ARGUMENTS": json.dumps(arguments, separators=(",", ":")),
        "NATIVE_FUNCTION": case.function,
        "NATIVE_FILENAME": case.source.name,
        "NATIVE_WAVE": str(case.wave),
        "NATIVE_TARGET": target,
        "NATIVE_REPEAT": "10",
        "NATIVE_FAULT": "",
    }


def verify_native_result(
    document: dict[str, Any], expected: Any, target: str
) -> None:
    if document.get("status") != "accepted" or document.get("phase") != "complete":
        raise AssertionError(json.dumps(document, indent=2))
    execution = document["execution"]
    checks = {
        "target": target,
        "target_triple": TARGETS[target][2],
        "status": "returned",
        "vm_handler_calls": 0,
        "executions": 10,
        "writable_after_publish": False,
        "executable_after_publish": True,
        "return_value": expected,
    }
    for key, value in checks.items():
        if execution.get(key) != value:
            raise AssertionError(
                f"{key}: expected {value!r}, got {execution.get(key)!r}"
            )
    if not execution.get("machine_code"):
        raise AssertionError("native execution did not expose emitted machine code")


def llvm_mc() -> str:
    candidates = (
        shutil.which("llvm-mc"),
        "/opt/homebrew/opt/llvm/bin/llvm-mc",
        "/usr/local/opt/llvm/bin/llvm-mc",
    )
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return candidate
    raise RuntimeError("llvm-mc is required for native disassembly verification")


def verify_disassembly(machine_code: str, target: str) -> None:
    raw = bytes.fromhex(machine_code)
    lines = [
        " ".join(f"0x{byte:02x}" for byte in raw[index:index + 4])
        for index in range(0, len(raw), 4)
    ]
    completed = subprocess.run(
        [llvm_mc(), "--disassemble", f"--triple={TARGETS[target][2]}"],
        input="\n".join(lines) + "\n",
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"llvm-mc disassembly failed: {completed.stderr.strip()}")
    if TARGETS[target][3] not in completed.stdout.lower():
        raise AssertionError(f"unexpected disassembly:\n{completed.stdout}")


def verify_platform(target: str) -> None:
    expected_system, expected_machine, _, _ = TARGETS[target]
    system = platform.system()
    machine = platform.machine()
    if (system, machine) != (expected_system, expected_machine):
        raise RuntimeError(
            f"{target} requires {expected_system} {expected_machine}; "
            f"host is {system} {machine}"
        )
    if target == "darwin-arm64-dev":
        translated = subprocess.run(
            ["sysctl", "-in", "sysctl.proc_translated"],
            text=True,
            capture_output=True,
            check=False,
        )
        value = translated.stdout.strip() if translated.returncode == 0 else "0"
        if value != "0":
            raise RuntimeError("Rosetta execution is not a valid Darwin target")


def verify_corpus_coverage() -> None:
    corpus_roots = (
        Path("tests/native/lowering/corpus/accepted"),
        Path("tests/native/control-flow/corpus/accepted"),
    )
    expected = {
        path.relative_to(ROOT)
        for root in corpus_roots
        for path in (ROOT / root).glob("*.php")
    }
    covered = {
        case.source
        for case in CASES
        if any(case.source.is_relative_to(root) for root in corpus_roots)
    }
    if covered != expected:
        missing = sorted(str(path) for path in expected - covered)
        stale = sorted(str(path) for path in covered - expected)
        raise RuntimeError(
            f"accepted corpus coverage drift: missing={missing}, stale={stale}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", required=True, choices=TARGETS)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument(
        "--reference", type=Path, default=Path(shutil.which("php") or "")
    )
    arguments = parser.parse_args()

    verify_platform(arguments.target)
    verify_corpus_coverage()
    candidate = arguments.candidate.resolve(strict=True)
    reference = arguments.reference.resolve(strict=True)
    if candidate == reference:
        raise RuntimeError("reference and candidate PHP binaries must differ")

    disassembly_sample: str | None = None
    checks = 0
    for case in CASES:
        for case_arguments in case.inputs:
            env = environment(case, case_arguments, arguments.target)
            expected = run_php(reference, REFERENCE_RUNNER, env)
            document = run_php(candidate, CANDIDATE_RUNNER, env)
            verify_native_result(document, expected, arguments.target)
            disassembly_sample = disassembly_sample or document["execution"][
                "machine_code"
            ]
            checks += 1

    assert disassembly_sample is not None
    verify_disassembly(disassembly_sample, arguments.target)

    first_env = environment(CASES[0], CASES[0].inputs[0], arguments.target)
    second_case = next(case for case in CASES if case.function == "w04_if_else_int")
    second_env = environment(second_case, second_case.inputs[0], arguments.target)
    first_env.update(
        {
            "NATIVE_SOURCE_TWO": second_env["NATIVE_SOURCE"],
            "NATIVE_ARGUMENTS_TWO": second_env["NATIVE_ARGUMENTS"],
            "NATIVE_FUNCTION_TWO": second_env["NATIVE_FUNCTION"],
            "NATIVE_FILENAME_TWO": second_env["NATIVE_FILENAME"],
        }
    )
    reset_results = run_php(candidate, RESET_RUNNER, first_env)
    if [item.get("status") for item in reset_results] != ["accepted", "accepted"]:
        raise AssertionError("compiler cleanup did not permit an independent module")
    if any(item["execution"]["vm_handler_calls"] for item in reset_results):
        raise AssertionError("compiler reset path reached a VM handler")

    fault_env = environment(CASES[0], CASES[0].inputs[0], arguments.target)
    fault_env["NATIVE_FAULT"] = "mapping_failure"
    fault = run_php(candidate, CANDIDATE_RUNNER, fault_env)
    if fault.get("phase") != "publish" or fault.get("status") != "error":
        raise AssertionError("mapping failure did not stop publication")
    if fault["execution"]["vm_handler_calls"] != 0:
        raise AssertionError("mapping failure reached a VM handler")

    invalid_env = environment(CASES[0], CASES[0].inputs[0], "invalid-target")
    invalid = run_php(candidate, CANDIDATE_RUNNER, invalid_env)
    if invalid.get("status") != "error":
        raise AssertionError("invalid target was accepted")

    print(
        f"PASS target={arguments.target} differential_cases={checks} "
        "repeat=10 vm_handler_calls=0 mapping_failure=clean disassembly=native"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
