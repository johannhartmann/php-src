#!/usr/bin/env python3
"""Execute the accepted W03-W08 runtime as native machine code."""

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


@dataclass(frozen=True)
class CallCase:
    identifier: str
    source: Path
    function: str
    inputs: tuple[Any, ...]
    compiler_mode: str | None = None


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

W07_REFERENCE_RUNNER = r"""
$source = base64_decode(getenv("NATIVE_SOURCE"), true);
$arguments = json_decode(getenv("NATIVE_ARGUMENTS"), true, 512, JSON_THROW_ON_ERROR);
$function = getenv("NATIVE_FUNCTION");
if ($source === false || !str_starts_with($source, "<?php")) {
    throw new RuntimeException("invalid source");
}
ob_start();
try {
    eval(substr($source, 5));
    $returnValue = $function(...$arguments);
    $output = ob_get_contents();
} finally {
    ob_end_clean();
}
echo json_encode(
    ["output" => $output, "return_value" => $returnValue],
    JSON_PRESERVE_ZERO_FRACTION | JSON_THROW_ON_ERROR
);
"""

W07_CANDIDATE_RUNNER = r"""
$source = base64_decode(getenv("NATIVE_SOURCE"), true);
$arguments = json_decode(getenv("NATIVE_ARGUMENTS"), true, 512, JSON_THROW_ON_ERROR);
$options = [
    "wave" => (int) getenv("NATIVE_WAVE"),
    "function" => getenv("NATIVE_FUNCTION"),
    "target" => getenv("NATIVE_TARGET"),
    "repeat" => (int) getenv("NATIVE_REPEAT"),
];
$compilerMode = getenv("NATIVE_COMPILER_MODE");
if ($compilerMode !== "") {
    $options["compiler_mode"] = $compilerMode;
}
if (getenv("NATIVE_STACK_PROBE") === "1") {
    $options["stack_probe"] = true;
}
ob_start();
try {
    $result = native_mir_test_compile_execute(
        $source, getenv("NATIVE_FILENAME"), $arguments, $options
    );
    $output = ob_get_contents();
} finally {
    ob_end_clean();
}
echo json_encode(
    ["output" => $output, "result" => $result],
    JSON_PRESERVE_ZERO_FRACTION | JSON_THROW_ON_ERROR
);
"""

W07_RESET_RUNNER = r"""
$source = base64_decode(getenv("NATIVE_SOURCE"), true);
$arguments = json_decode(getenv("NATIVE_ARGUMENTS"), true, 512, JSON_THROW_ON_ERROR);
$options = [
    "wave" => 7,
    "function" => getenv("NATIVE_FUNCTION"),
    "target" => getenv("NATIVE_TARGET"),
];
ob_start();
$first = native_mir_test_compile_execute(
    $source, getenv("NATIVE_FILENAME"), $arguments, $options
);
$second = native_mir_test_compile_execute(
    $source, getenv("NATIVE_FILENAME"), $arguments, $options
);
ob_end_clean();
echo json_encode([$first, $second], JSON_PRESERVE_ZERO_FRACTION | JSON_THROW_ON_ERROR);
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


def w05_accepted_cases() -> tuple[CallCase, ...]:
    manifest = json.loads(
        (ROOT / "tests/native/calls/corpus/manifest.json").read_text()
    )
    inputs = {
        "direct_call_in_if": (True,),
        "direct_call_in_reducible_loop": (False,),
        "direct_call_after_phi": (False, True, True),
    }
    return tuple(
        CallCase(
            entry["id"],
            Path(entry["source"]),
            entry["function"],
            inputs.get(entry["id"], ()),
            entry.get("compiler_mode"),
        )
        for entry in manifest["cases"]
        if entry["status"] == "accepted"
    )


def w07_environment(
    source: bytes,
    filename: str,
    function: str,
    arguments: tuple[Any, ...],
    target: str,
    *,
    compiler_mode: str | None = None,
    wave: int = 7,
    repeat: int = 1,
    stack_probe: bool = False,
) -> dict[str, str]:
    return {
        "NATIVE_SOURCE": base64.b64encode(source).decode("ascii"),
        "NATIVE_ARGUMENTS": json.dumps(arguments, separators=(",", ":")),
        "NATIVE_FUNCTION": function,
        "NATIVE_FILENAME": filename,
        "NATIVE_TARGET": target,
        "NATIVE_WAVE": str(wave),
        "NATIVE_REPEAT": str(repeat),
        "NATIVE_COMPILER_MODE": compiler_mode or "",
        "NATIVE_STACK_PROBE": "1" if stack_probe else "0",
    }


def verify_no_vm_dispatch(execution: dict[str, Any]) -> None:
    for counter in (
        "vm_handler_calls",
        "execute_ex_calls",
        "opline_handler_calls",
    ):
        if execution.get(counter) != 0:
            raise AssertionError(
                f"{counter}: expected 0, got {execution.get(counter)!r}"
            )


def verify_w07_result(
    document: dict[str, Any],
    expected: dict[str, Any],
    target: str,
    *,
    executions: int = 1,
) -> dict[str, Any]:
    result = document.get("result")
    if not isinstance(result, dict):
        raise AssertionError(json.dumps(document, indent=2))
    if result.get("status") != "accepted" or result.get("phase") != "complete":
        raise AssertionError(json.dumps(result, indent=2))
    if document.get("output") != expected.get("output"):
        raise AssertionError(
            f"output: expected {expected.get('output')!r}, "
            f"got {document.get('output')!r}"
        )
    execution = result["execution"]
    checks = {
        "target": target,
        "target_triple": TARGETS[target][2],
        "status": "returned",
        "executions": executions,
        "writable_after_publish": False,
        "executable_after_publish": True,
        "return_value": expected.get("return_value"),
    }
    for key, value in checks.items():
        if execution.get(key) != value:
            raise AssertionError(
                f"{key}: expected {value!r}, got {execution.get(key)!r}"
            )
    verify_no_vm_dispatch(execution)
    if not execution.get("machine_code"):
        raise AssertionError("W07 execution did not publish machine code")
    return execution


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
    verify_no_vm_dispatch(execution)


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


def mixed_argument_source(count: int) -> bytes:
    names = [f"$p{index}" for index in range(count)]
    values = ("1", "true", "2.5", "null")
    arguments = [values[index % len(values)] for index in range(count)]
    return (
        "<?php\n"
        f"function boundary_target_{count}({', '.join(names)}): void "
        "{ echo $p0; }\n"
        f"function boundary_case_{count}(): void {{ "
        f"boundary_target_{count}({', '.join(arguments)}); }}\n"
    ).encode()


def deep_recursion_source(depth: int) -> tuple[bytes, tuple[bool, ...]]:
    parameters = ", ".join(f"bool $a{index}" for index in range(depth))
    recursive_arguments = ", ".join(
        [*(f"$a{index}" for index in range(1, depth)), "false"]
    )
    source = (
        "<?php\n"
        f"function deep_recursive({parameters}): bool {{\n"
        "    if (!$a0) { return true; }\n"
        f"    return deep_recursive({recursive_arguments});\n"
        "}\n"
    )
    return source.encode(), (True,) * depth


STACK_PROBE_SOURCE = b"""<?php
function probe_leaf(int $value): int
{
    echo 1;
    return $value;
}
function probe_middle(): int
{
    echo 2;
    return probe_leaf(7);
}
function probe_root(): int
{
    echo 3;
    return probe_middle();
}
"""


MUTUAL_RECURSION_SOURCE = b"""<?php
function mutual_even(bool $again): bool
{
    if (!$again) { return true; }
    return mutual_odd(false);
}
function mutual_odd(bool $again): bool
{
    if (!$again) { return false; }
    return mutual_even(false);
}
function mutual_case(): bool
{
    return mutual_even(true);
}
"""


W08_INTERNAL_VALUE_SOURCE = b"""<?php
function native_internal_value(mixed $value): string
{
    return strrev(json_encode($value));
}
"""


W08_EXCEPTION_SOURCE = b"""<?php
function native_internal_exception(): int
{
    try {
        intdiv(1, 0);
    } catch (DivisionByZeroError) {
        return 6;
    }
    return 0;
}
"""


W08_REENTRY_SOURCE = b"""<?php
function native_internal_callback(int $value): int
{
    return $value;
}
function native_internal_reentry(): int
{
    array_map('native_internal_callback', [4]);
    return 7;
}
"""


W08_BYREF_SOURCE = b"""<?php
function native_internal_byref(string $value, int $count): int
{
    str_replace('a', 'b', $value, $count);
    return strcmp(json_encode($count), '2');
}
"""


def run_stack_limit_case(
    binary: Path, program: str, env: dict[str, str]
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            str(binary),
            "-n",
            "-d",
            "zend.max_allowed_stack_size=1048576",
            "-d",
            "display_errors=1",
            "-d",
            "log_errors=0",
            "-r",
            program,
        ],
        cwd=ROOT,
        env={**os.environ, **env},
        text=True,
        capture_output=True,
        timeout=30,
        check=False,
    )


def verify_infinite_self_recursion(
    candidate: Path, target: str, case: CallCase
) -> None:
    source = (ROOT / case.source).read_bytes()
    env = w07_environment(
        source, case.source.name, case.function, case.inputs, target
    )
    candidate_program = r"""
$source = base64_decode(getenv("NATIVE_SOURCE"), true);
native_mir_test_compile_execute(
    $source,
    getenv("NATIVE_FILENAME"),
    [],
    ["wave" => 7, "function" => getenv("NATIVE_FUNCTION"),
     "target" => getenv("NATIVE_TARGET")]
);
"""
    completed = run_stack_limit_case(candidate, candidate_program, env)
    output = completed.stdout + completed.stderr
    if completed.returncode == 0 or "Maximum call stack size" not in output:
        raise AssertionError(
            "native self-recursion did not reach the Zend stack limit: "
            f"exit={completed.returncode} output={output[-1000:]!r}"
        )
    if case.source.name not in output:
        raise AssertionError("native recursion stack trace lost its source filename")


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

    call_cases = w05_accepted_cases()
    recursive_case = next(
        case for case in call_cases if case.identifier == "recursive_self_call"
    )
    for case in call_cases:
        if case is recursive_case:
            continue
        source = (ROOT / case.source).read_bytes()
        env = w07_environment(
            source,
            case.source.name,
            case.function,
            case.inputs,
            arguments.target,
            compiler_mode=case.compiler_mode,
        )
        expected = run_php(reference, W07_REFERENCE_RUNNER, env)
        document = run_php(candidate, W07_CANDIDATE_RUNNER, env)
        execution = verify_w07_result(document, expected, arguments.target)
        disassembly_sample = disassembly_sample or execution["machine_code"]
        checks += 1

    verify_infinite_self_recursion(candidate, arguments.target, recursive_case)
    checks += 1

    for count in (1, 6, 7, 8, 9, 16, 128):
        source = mixed_argument_source(count)
        function = f"boundary_case_{count}"
        env = w07_environment(
            source,
            f"boundary-{count}.php",
            function,
            (),
            arguments.target,
            stack_probe=True,
        )
        expected = run_php(reference, W07_REFERENCE_RUNNER, env)
        document = run_php(candidate, W07_CANDIDATE_RUNNER, env)
        execution = verify_w07_result(document, expected, arguments.target)
        trace = execution.get("stack_trace")
        expected_types = [
            ("int", "bool", "float", "null")[index % 4]
            for index in range(count)
        ]
        if not execution.get("frame_chain_valid") or not isinstance(trace, list):
            raise AssertionError("argument boundary probe did not retain frames")
        matching_frames = [
            frame for frame in trace
            if frame.get("callee") == f"boundary_target_{count}"
        ]
        if len(matching_frames) != 1:
            raise AssertionError(
                f"argument boundary probe count mismatch: {matching_frames!r}"
            )
        frame = matching_frames[0]
        if frame.get("argument_count") != count \
                or frame.get("argument_types") != expected_types:
            raise AssertionError(
                f"argument boundary frame mismatch: {frame!r}"
            )
        checks += 1

    mutual_env = w07_environment(
        MUTUAL_RECURSION_SOURCE,
        "mutual.php",
        "mutual_case",
        (),
        arguments.target,
    )
    mutual_expected = run_php(reference, W07_REFERENCE_RUNNER, mutual_env)
    mutual_document = run_php(candidate, W07_CANDIDATE_RUNNER, mutual_env)
    verify_w07_result(mutual_document, mutual_expected, arguments.target)
    checks += 1

    deep_source, deep_arguments = deep_recursion_source(128)
    deep_env = w07_environment(
        deep_source,
        "deep-recursion.php",
        "deep_recursive",
        deep_arguments,
        arguments.target,
        stack_probe=True,
    )
    deep_expected = run_php(reference, W07_REFERENCE_RUNNER, deep_env)
    deep_document = run_php(candidate, W07_CANDIDATE_RUNNER, deep_env)
    deep_execution = verify_w07_result(
        deep_document, deep_expected, arguments.target
    )
    if not deep_execution.get("frame_chain_valid") \
            or len(deep_execution.get("stack_trace", ())) != 128:
        raise AssertionError("deep native recursion did not retain 128 Zend frames")
    checks += 1

    probe_env = w07_environment(
        STACK_PROBE_SOURCE,
        "stack-probe.php",
        "probe_root",
        (),
        arguments.target,
        stack_probe=True,
    )
    probe_expected = run_php(reference, W07_REFERENCE_RUNNER, probe_env)
    probe_document = run_php(candidate, W07_CANDIDATE_RUNNER, probe_env)
    probe_execution = verify_w07_result(
        probe_document, probe_expected, arguments.target
    )
    trace = probe_execution.get("stack_trace")
    expected_trace = [
        ("probe_root", "probe_middle", 15),
        ("probe_middle", "probe_leaf", 10),
    ]
    if not probe_execution.get("frame_chain_valid") or not isinstance(trace, list):
        raise AssertionError("native stack probe did not retain the Zend frame chain")
    observed_trace = [
        (frame.get("caller"), frame.get("callee"), frame.get("caller_line"))
        for frame in trace
    ]
    if observed_trace != expected_trace or not all(
        frame.get("previous_matches_caller") for frame in trace
    ):
        raise AssertionError(
            f"native stack source mapping mismatch: {observed_trace!r}"
        )
    checks += 1

    w08_cases = (
        (
            "w08-internal-value.php",
            W08_INTERNAL_VALUE_SOURCE,
            "native_internal_value",
            ((None,), (True,), (7,), (1.5,), ("hello",), ([1, 2],)),
        ),
        (
            "w08-internal-exception.php",
            W08_EXCEPTION_SOURCE,
            "native_internal_exception",
            ((),),
        ),
        (
            "w08-internal-reentry.php",
            W08_REENTRY_SOURCE,
            "native_internal_reentry",
            ((),),
        ),
        (
            "w08-internal-byref.php",
            W08_BYREF_SOURCE,
            "native_internal_byref",
            (("a-a", 0),),
        ),
    )
    for filename, source, function, inputs in w08_cases:
        for case_arguments in inputs:
            env = w07_environment(
                source,
                filename,
                function,
                case_arguments,
                arguments.target,
                wave=8,
            )
            expected = run_php(reference, W07_REFERENCE_RUNNER, env)
            document = run_php(candidate, W07_CANDIDATE_RUNNER, env)
            execution = verify_w07_result(document, expected, arguments.target)
            disassembly_sample = disassembly_sample or execution["machine_code"]
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

    repeat_case = next(
        case for case in call_cases
        if case.identifier == "direct_user_scalar_result_followup"
    )
    repeat_source = (ROOT / repeat_case.source).read_bytes()
    repeat_env = w07_environment(
        repeat_source,
        repeat_case.source.name,
        repeat_case.function,
        repeat_case.inputs,
        arguments.target,
    )
    repeated_compilations = run_php(candidate, W07_RESET_RUNNER, repeat_env)
    if [item.get("status") for item in repeated_compilations] \
            != ["accepted", "accepted"]:
        raise AssertionError("W07 entry cells failed across repeated compilations")
    for item in repeated_compilations:
        verify_no_vm_dispatch(item["execution"])

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
        "repeat=10 frame_depth=128 vm_handler_calls=0 execute_ex_calls=0 "
        "opline_handler_calls=0 mapping_failure=clean disassembly=native"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
