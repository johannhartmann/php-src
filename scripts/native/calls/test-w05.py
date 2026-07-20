#!/usr/bin/env python3
"""Run the W05 modeled/deferred, determinism, fault, and semantic gate."""

from __future__ import annotations

import argparse
import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
MANIFEST = ROOT / "tests/native/calls/corpus/manifest.json"
GOLDEN_ROOT = ROOT / "tests/native/calls/integration/goldens"
GOLDEN_INDEX = GOLDEN_ROOT / "index.json"
EVIDENCE = ROOT / "tests/native/calls/integration/gate-evidence.json"
DUMP_PATH = ROOT / "scripts/native/calls/dump-w05.py"
ARENA_SIZES = (64, 4096, 65536)
FAULTS = {
    "compile_bailout": ("error", "compile", "BAILOUT"),
    "ssa_failure": ("rejected", "ssa", "SSA0001"),
    "lower_failure": ("error", "lowering", "MIRL0007"),
    "module_oom": ("error", "lowering", "MIRL0007"),
    "planner_allocation": ("error", "lowering", "MIRL0028"),
    "target_snapshot": ("error", "lowering", "MIRL0028"),
    "argument_table": ("error", "lowering", "MIRL0028"),
    "frame_state": ("error", "lowering", "MIRL0028"),
    "call_record": ("error", "lowering", "MIRL0028"),
    "finalize_failure": ("error", "lowering", "MIRL0028"),
    "stage1_verifier_failure": ("error", "lowering", "MIRL0009"),
    "stage2_verifier_failure": ("error", "lowering", "MIRL0010"),
    "structural_verifier_failure": ("error", "lowering", "MIRL0029"),
    "scalar_verifier_failure": ("error", "lowering", "MIRL0029"),
    "control_flow_verifier_failure": ("error", "lowering", "MIRL0029"),
    "call_verifier_failure": ("error", "lowering", "MIRL0029"),
    "fingerprint_recompute_failure": ("error", "lowering", "MIRL0029"),
    "dump_failure": ("error", "dump", "MIRV0011"),
}


class W05TestError(RuntimeError):
    """The W05 gate failed to produce trustworthy evidence."""


def load_dump() -> Any:
    specification = importlib.util.spec_from_file_location("w05_gate_dump", DUMP_PATH)
    if specification is None or specification.loader is None:
        raise W05TestError("unable to load dump-w05.py")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


dump_w05 = load_dump()


def stable_bytes(value: Any) -> bytes:
    return (
        json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    ).encode("utf-8")


def stable_write(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(stable_bytes(value))
    os.replace(temporary, path)


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def executable(value: str, label: str) -> Path:
    path = Path(value).expanduser()
    if not path.is_absolute():
        raise W05TestError(f"{label} PHP path must be absolute")
    path = path.resolve()
    if not path.is_file() or not os.access(path, os.X_OK):
        raise W05TestError(f"{label} PHP is not executable: {value}")
    return path


def environment(sanitizer: str | None) -> dict[str, str]:
    result = os.environ.copy()
    result.update(
        {
            "LANG": "C",
            "LC_ALL": "C",
            "TZ": "UTC",
            "SOURCE_DATE_EPOCH": "0",
            "PYTHONDONTWRITEBYTECODE": "1",
            "ASAN_OPTIONS": "abort_on_error=1:detect_leaks=0",
            "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
        }
    )
    if sanitizer is not None:
        result["W05_SANITIZER"] = sanitizer
    return result


def execute(binary: Path, source: Path, env: dict[str, str]) -> bytes:
    completed = subprocess.run(
        [str(binary), "-n", str(source)],
        cwd=ROOT,
        env=env,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
        timeout=30,
    )
    return str(completed.returncode).encode() + b"\0" + completed.stdout


def invoke_with_axes(
    candidate: Path,
    source: bytes,
    filename: str,
    function: str,
    *,
    repeat: int,
    arena_size: int,
    opcache: bool,
    env: dict[str, str],
) -> dict[str, Any]:
    payload = base64.b64encode(source).decode("ascii")
    program = (
        "$source=base64_decode(" + json.dumps(payload) + ",true);"
        "$options=['wave'=>5,'function'=>" + json.dumps(function)
        + ",'arena_chunk_size'=>" + str(arena_size) + "];"
        "$calls=[];"
        f"for($i=0;$i<{repeat};$i++){{$calls[]=native_mir_test_compile_dump("
        "$source," + json.dumps(filename) + ",$options);}"
        "echo json_encode(['schema_version'=>1,'calls'=>$calls],"
        "JSON_UNESCAPED_SLASHES|JSON_THROW_ON_ERROR);"
    )
    command = [
        str(candidate),
        "-n",
        "-d",
        "opcache.enable_cli=" + ("1" if opcache else "0"),
        "-r",
        program,
    ]
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=env,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=60,
    )
    if completed.returncode != 0:
        raise W05TestError(
            "axis invocation failed: "
            + completed.stderr.decode("utf-8", errors="replace")
        )
    try:
        result = json.loads(completed.stdout)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise W05TestError("axis invocation returned invalid JSON") from error
    if len(result.get("calls", [])) != repeat:
        raise W05TestError("axis invocation returned the wrong call count")
    return result


def properties(mir: str) -> dict[str, int]:
    prefixes = {
        "arguments": "call-argument ",
        "continuations": "call-continuation ",
        "frames": "frame ",
        "receipts": "call-receipt ",
        "sites": "call-site ",
        "targets": "call-target ",
    }
    lines = mir.splitlines()
    return {
        name: sum(line.startswith(prefix) for line in lines)
        for name, prefix in prefixes.items()
    }


def validate_modeled(case: dict[str, Any], result: dict[str, Any]) -> str:
    codes = {item.get("code") for item in result.get("diagnostics", [])}
    if result.get("status") != "accepted" or case["mirl"] not in codes:
        raise W05TestError(f"{case['id']}: modeled status/diagnostic mismatch")
    mir = result.get("mir")
    if not isinstance(mir, str) or not mir.endswith("end\n"):
        raise W05TestError(f"{case['id']}: incomplete modeled MIR")
    required = (
        "opcode call_direct_user",
        "call-target ",
        "call-site ",
        "call-receipt ",
        "modeled true codegen-eligible false",
    )
    if any(token not in mir for token in required):
        raise W05TestError(f"{case['id']}: incomplete W05 call model")
    return mir


def validate_deferred(case: dict[str, Any], result: dict[str, Any]) -> None:
    codes = {item.get("code") for item in result.get("diagnostics", [])}
    if (
        result.get("status") != "rejected"
        or case["mirl"] not in codes
        or result.get("mir") is not None
        or not isinstance(case.get("wave"), str)
    ):
        raise W05TestError(f"{case['id']}: deferred classification mismatch")


def write_goldens(
    accepted: list[tuple[dict[str, Any], str]],
) -> dict[str, Any]:
    expected_names: set[str] = set()
    entries: dict[str, Any] = {}
    GOLDEN_ROOT.mkdir(parents=True, exist_ok=True)
    for case, mir in accepted:
        path = GOLDEN_ROOT / f"{case['id']}.znmir"
        payload = mir.encode("utf-8")
        path.write_bytes(payload)
        expected_names.add(path.name)
        entries[case["id"]] = {
            "path": path.relative_to(ROOT).as_posix(),
            "properties": properties(mir),
            "sha256": sha256_bytes(payload),
        }
    for path in GOLDEN_ROOT.glob("*.znmir"):
        if path.name not in expected_names:
            path.unlink()
    document = {
        "cases": entries,
        "format_version": "1.0.0",
        "normalization": False,
    }
    stable_write(GOLDEN_INDEX, document)
    return document


def verify_goldens(accepted: list[tuple[dict[str, Any], str]]) -> dict[str, Any]:
    try:
        index = json.loads(GOLDEN_INDEX.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise W05TestError(f"golden index: {error}") from error
    expected = {case["id"]: mir for case, mir in accepted}
    if (
        index.get("format_version") != "1.0.0"
        or index.get("normalization") is not False
        or set(index.get("cases", {})) != set(expected)
    ):
        raise W05TestError("golden index shape/case set mismatch")
    for case_id, mir in expected.items():
        entry = index["cases"][case_id]
        path = ROOT / entry["path"]
        payload = mir.encode("utf-8")
        if (
            not path.is_file()
            or path.read_bytes() != payload
            or entry.get("sha256") != sha256_bytes(payload)
            or entry.get("properties") != properties(mir)
        ):
            raise W05TestError(f"{case_id}: integration golden drift")
    return index


def verify_artifacts_only() -> dict[str, Any]:
    try:
        evidence = json.loads(EVIDENCE.read_text(encoding="utf-8"))
        index = json.loads(GOLDEN_INDEX.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise W05TestError(f"committed gate artifacts: {error}") from error
    if evidence.get("status") != "pass" or evidence.get("mir_executed") is not False:
        raise W05TestError("gate evidence does not record a non-executing pass")
    for case_id, entry in index.get("cases", {}).items():
        path = ROOT / entry["path"]
        if not path.is_file() or sha256_bytes(path.read_bytes()) != entry["sha256"]:
            raise W05TestError(f"{case_id}: committed golden digest mismatch")
    if evidence.get("golden_index_sha256") != sha256_bytes(
        GOLDEN_INDEX.read_bytes()
    ):
        raise W05TestError("gate evidence golden index digest mismatch")
    return evidence


def run_gate(
    reference: Path,
    candidate: Path,
    sanitizer: str | None,
    write: bool,
) -> dict[str, Any]:
    env = environment(sanitizer)
    for name in (
        "LANG",
        "LC_ALL",
        "TZ",
        "SOURCE_DATE_EPOCH",
        "PYTHONDONTWRITEBYTECODE",
        "ASAN_OPTIONS",
        "UBSAN_OPTIONS",
    ):
        os.environ[name] = env[name]
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    accepted: list[tuple[dict[str, Any], str]] = []
    deferred: list[dict[str, Any]] = []
    execution_count = 0
    for case in manifest["cases"]:
        source = ROOT / case["source"]
        if execute(reference, source, env) != execute(candidate, source, env):
            raise W05TestError(f"{case['id']}: reference execution mismatch")
        execution_count += 1
        document = dump_w05.invoke(
            candidate,
            source.read_bytes(),
            case["source"],
            case["function"],
            repeat=2,
            timeout=60,
            compiler_mode=case.get("compiler_mode"),
        )
        first, second = document["calls"]
        if first != second:
            raise W05TestError(f"{case['id']}: repeated dump differs")
        if case["status"] == "accepted":
            accepted.append((case, validate_modeled(case, first)))
        else:
            validate_deferred(case, first)
            deferred.append(case)

    exemplar_case, exemplar_mir = accepted[0]
    exemplar_source = (ROOT / exemplar_case["source"]).read_bytes()
    for repeat in range(1, 11):
        document = invoke_with_axes(
            candidate,
            exemplar_source,
            exemplar_case["source"],
            exemplar_case["function"],
            repeat=repeat,
            arena_size=4096,
            opcache=False,
            env=env,
        )
        if any(
            validate_modeled(exemplar_case, item) != exemplar_mir
            for item in document["calls"]
        ):
            raise W05TestError("repeat count changed canonical MIR")

    for arena_size in ARENA_SIZES:
        for opcache in (False, True):
            document = invoke_with_axes(
                candidate,
                exemplar_source,
                exemplar_case["source"],
                exemplar_case["function"],
                repeat=2,
                arena_size=arena_size,
                opcache=opcache,
                env=env,
            )
            if any(
                validate_modeled(exemplar_case, item) != exemplar_mir
                for item in document["calls"]
            ):
                raise W05TestError("determinism axis changed canonical MIR")

    alternate_identity = invoke_with_axes(
        candidate,
        exemplar_source,
        "w05-source-identity-alternate.php",
        exemplar_case["function"],
        repeat=2,
        arena_size=4096,
        opcache=False,
        env=env,
    )
    if any(
        validate_modeled(exemplar_case, item) != exemplar_mir
        for item in alternate_identity["calls"]
    ):
        raise W05TestError("source identity changed canonical MIR")

    for case, mir in accepted:
        call_site_ids = [
            line.split()[1]
            for line in mir.splitlines()
            if line.startswith("call-site ")
        ]
        expected_ids = [f"cs{index}" for index in range(len(call_site_ids))]
        if call_site_ids != expected_ids:
            raise W05TestError(
                f"{case['id']}: call-site table enumeration is unstable"
            )

    fault_results = {}
    fault_case = next(
        (
            case
            for case, _mir in accepted
            if case["id"] == "direct_user_scalar_literals"
        ),
        None,
    )
    if fault_case is None:
        raise W05TestError("fault exemplar is missing from accepted corpus")
    fault_source = (ROOT / fault_case["source"]).read_bytes()
    for fault, (status, phase, code) in FAULTS.items():
        result = dump_w05.invoke(
            candidate,
            fault_source,
            fault_case["source"],
            fault_case["function"],
            repeat=2,
            timeout=60,
            fault=fault,
        )["calls"]
        for item in result:
            codes = {entry.get("code") for entry in item.get("diagnostics", [])}
            if (
                item.get("status") != status
                or item.get("phase") != phase
                or code not in codes
                or item.get("mir") is not None
            ):
                raise W05TestError(f"{fault}: failure atomicity mismatch")
        fault_results[fault] = {"diagnostic": code, "phase": phase}

    index = write_goldens(accepted) if write else verify_goldens(accepted)
    wave_counts: dict[str, int] = {}
    diagnostic_counts: dict[str, int] = {}
    for case in manifest["cases"]:
        diagnostic_counts[case["mirl"]] = diagnostic_counts.get(case["mirl"], 0) + 1
        if case.get("wave"):
            wave_counts[case["wave"]] = wave_counts.get(case["wave"], 0) + 1
    evidence = {
        "accepted_case_count": len(accepted),
        "candidate_kind": (
            sanitizer if sanitizer is not None else "debug"
        ),
        "deferred_by_wave": dict(sorted(wave_counts.items())),
        "deferred_case_count": len(deferred),
        "determinism_axes": [
            "repeat-1-through-10",
            "arena-chunk-size",
            "opcache-on-off",
            "call-site-enumeration",
            "source-identity",
        ],
        "diagnostic_coverage": dict(sorted(diagnostic_counts.items())),
        "execution_comparisons": execution_count,
        "fault_atomicity": fault_results,
        "format_version": "1.0.0",
        "golden_index_sha256": sha256_bytes(stable_bytes(index)),
        "mir_executed": False,
        "normalization": False,
        "status": "pass",
    }
    if write:
        stable_write(EVIDENCE, evidence)
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference-php")
    parser.add_argument("--candidate-php")
    parser.add_argument("--sanitizer", choices=["address", "undefined"])
    parser.add_argument("--write-goldens", action="store_true")
    parser.add_argument("--verify-artifacts-only", action="store_true")
    parser.add_argument("--json-out", type=Path)
    arguments = parser.parse_args()
    try:
        if arguments.verify_artifacts_only:
            evidence = verify_artifacts_only()
        else:
            if not arguments.reference_php or not arguments.candidate_php:
                parser.error("--reference-php and --candidate-php are required")
            evidence = run_gate(
                executable(arguments.reference_php, "reference"),
                executable(arguments.candidate_php, "candidate"),
                arguments.sanitizer,
                arguments.write_goldens,
            )
        if arguments.json_out is not None:
            stable_write(arguments.json_out, evidence)
        print(
            "W05 gate test: PASS accepted={} deferred={} mir_executed=false".format(
                evidence["accepted_case_count"],
                evidence["deferred_case_count"],
            )
        )
        return 0
    except (
        W05TestError,
        OSError,
        subprocess.TimeoutExpired,
        json.JSONDecodeError,
    ) as error:
        print(f"W05 gate test: FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
