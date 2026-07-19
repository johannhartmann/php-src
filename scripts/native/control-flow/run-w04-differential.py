#!/usr/bin/env python3
"""Run byte-exact PHP execution and a separate W04 compile/dump channel."""

from __future__ import annotations

import argparse
import base64
import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
CORPUS = ROOT / "tests/native/control-flow/corpus"
DEFAULT_MANIFEST = CORPUS / "manifest.json"
VALIDATOR_PATH = CORPUS / "validate_manifest.py"
DUMP_PATH = ROOT / "scripts/native/control-flow/dump-w04.py"

EXIT_PASS = 0
EXIT_MISMATCH = 1
EXIT_HARNESS_ERROR = 2
SELF_TEST_TIMEOUT = 15.0


class DifferentialError(ValueError):
    """The differential harness cannot produce trustworthy W04 evidence."""


def load_module(name: str, path: Path) -> Any:
    specification = importlib.util.spec_from_file_location(name, path)
    if specification is None or specification.loader is None:
        raise DifferentialError("unable to load {}".format(path))
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


dump_w04 = load_module("w04_dump", DUMP_PATH)
manifest_validator = load_module("w04_manifest_validator", VALIDATOR_PATH)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_binary(value: str, label: str) -> Path:
    candidate = Path(value).expanduser()
    if not candidate.is_absolute():
        raise DifferentialError("{} PHP path must be absolute".format(label))
    resolved = candidate.resolve()
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise DifferentialError("{} PHP is not executable: {}".format(label, value))
    return resolved


def deterministic_environment(
    base: dict[str, str] | None = None,
) -> dict[str, str]:
    environment = (base if base is not None else os.environ).copy()
    environment.update(
        {
            "LANG": "C",
            "LC_ALL": "C",
            "TZ": "UTC",
            "SOURCE_DATE_EPOCH": "0",
            "W04_SEED": "20260719",
        }
    )
    return environment


def binary_identity(path: Path) -> dict[str, Any]:
    return {
        "path": str(path),
        "sha256": sha256_file(path),
        "size": path.stat().st_size,
    }


def process_json(completed: subprocess.CompletedProcess[bytes]) -> dict[str, Any]:
    return {
        "exit_code": completed.returncode if completed.returncode >= 0 else None,
        "signal": -completed.returncode if completed.returncode < 0 else None,
        "stderr_base64": base64.b64encode(completed.stderr).decode("ascii"),
        "stderr_sha256": hashlib.sha256(completed.stderr).hexdigest(),
        "stdout_base64": base64.b64encode(completed.stdout).decode("ascii"),
        "stdout_sha256": hashlib.sha256(completed.stdout).hexdigest(),
    }


def execute_source(
    binary: Path,
    source: Path,
    timeout: float,
    environment: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[bytes]:
    try:
        return subprocess.run(
            [
                str(binary),
                "-n",
                "-d",
                "date.timezone=UTC",
                "-d",
                "display_errors=stderr",
                str(source),
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=timeout,
            env=deterministic_environment(environment),
            cwd=source.parent,
        )
    except subprocess.TimeoutExpired as error:
        raise DifferentialError("source execution timed out: {}".format(source)) from error
    except OSError as error:
        raise DifferentialError("source execution failed: {}".format(error)) from error


def execution_differences(
    reference: subprocess.CompletedProcess[bytes],
    candidate: subprocess.CompletedProcess[bytes],
) -> list[str]:
    differences = []
    if reference.stdout != candidate.stdout:
        differences.append("stdout")
    if reference.stderr != candidate.stderr:
        differences.append("stderr")
    if reference.returncode != candidate.returncode:
        differences.append("termination")
    return differences


def diagnostic_codes(result: dict[str, Any]) -> list[str]:
    return [item["code"] for item in result["diagnostics"]]


def mir_cfg_properties(mir: str) -> dict[str, Any]:
    blocks = []
    edge_count = 0
    backedge_count = 0
    phi_count = 0
    for line in mir.splitlines():
        block = re.match(r"^block b([0-9]+)\b.*\bsuccessors \[([^\]]*)\]", line)
        if block is not None:
            source_id = int(block.group(1))
            blocks.append(source_id)
            successors = [int(item) for item in re.findall(r"\bb([0-9]+)\b", block.group(2))]
            edge_count += len(successors)
            backedge_count += sum(target <= source_id for target in successors)
        if re.match(r"^instruction i[0-9]+\b.*\bopcode phi\b", line):
            phi_count += 1
    return {
        "backedges": backedge_count,
        "blocks": len(blocks),
        "edges": edge_count,
        "phis": phi_count,
    }


def cfg_differences(case: dict[str, Any], mir: str, call: int) -> list[str]:
    expected = case["expected_cfg"]
    actual = mir_cfg_properties(mir)
    differences = []
    for expected_key, actual_key in (
        ("min_blocks", "blocks"),
        ("min_edges", "edges"),
        ("min_phis", "phis"),
    ):
        if actual[actual_key] < expected[expected_key]:
            differences.append("{}_call_{}".format(actual_key, call))
    if expected["requires_backedge"] and actual["backedges"] < 1:
        differences.append("backedge_call_{}".format(call))
    if expected["requires_loop"] and actual["backedges"] < 1:
        differences.append("loop_call_{}".format(call))
    return differences


def check_dump(
    case: dict[str, Any],
    candidate: Path,
    source: Path,
    timeout: float,
) -> tuple[dict[str, Any], list[str]]:
    repeat = max(case["repeat_calls"])
    document, _ = dump_w04.invoke(
        str(candidate),
        source.read_bytes(),
        case["source_path"],
        function=case["function"],
        repeat=repeat,
        timeout=timeout,
    )
    differences = []
    if document["status"] != case["expected_status"]:
        differences.append("dump_status")
    selected_calls = []
    for call_number in case["repeat_calls"]:
        call = document["calls"][call_number - 1]
        selected_calls.append(call)
        result = call["result"]
        if case["expected_mirl"] not in diagnostic_codes(result):
            differences.append("diagnostic_call_{}".format(call_number))
        if case["expected_status"] == "accepted":
            if result["mir"] is not None:
                differences.extend(cfg_differences(case, result["mir"], call_number))
        elif result["mir"] is not None:
            differences.append("partial_mir_call_{}".format(call_number))
    return (
        {
            "calls": selected_calls,
            "normalization": document["normalization"],
            "status": document["status"],
            "wave": document["wave"],
        },
        sorted(set(differences)),
    )


def run_case(
    case: dict[str, Any],
    reference: Path,
    candidate: Path,
    timeout: float,
) -> dict[str, Any]:
    source = ROOT / case["source_path"]
    reference_run = execute_source(reference, source, timeout)
    candidate_run = execute_source(candidate, source, timeout)
    differences = execution_differences(reference_run, candidate_run)
    dump, dump_differences = check_dump(case, candidate, source, timeout)
    differences.extend(dump_differences)
    return {
        "case_id": case["case_id"],
        "differences": sorted(set(differences)),
        "dump": dump,
        "execution": {
            "candidate": process_json(candidate_run),
            "comparison": "byte_exact",
            "normalization": {"enabled": False, "rules": []},
            "reference": process_json(reference_run),
        },
        "reference_inputs": case["reference_inputs"],
        "source_sha256": case["source_sha256"],
        "status": "pass" if not differences else "mismatch",
    }


def repository_commit() -> str | None:
    completed = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        check=False,
    )
    return completed.stdout.strip() if completed.returncode == 0 else None


def run(
    reference_value: str,
    candidate_value: str,
    manifest_path: Path,
    timeout: float,
) -> tuple[dict[str, Any], int]:
    if timeout <= 0:
        raise DifferentialError("timeout must be greater than zero")
    reference = canonical_binary(reference_value, "reference")
    candidate = canonical_binary(candidate_value, "candidate")
    if os.path.samefile(reference, candidate):
        raise DifferentialError("reference and candidate PHP must be distinct paths")
    manifest_path = manifest_path.resolve()
    try:
        manifest_relative = manifest_path.relative_to(ROOT)
    except ValueError as error:
        raise DifferentialError("manifest must be inside the repository") from error
    manifest = manifest_validator.load_json(manifest_path)
    manifest_validator.validate_document(manifest)
    cases = [
        run_case(case, reference, candidate, timeout)
        for case in manifest["cases"]
    ]
    mismatch_count = sum(case["status"] == "mismatch" for case in cases)
    exit_code = EXIT_PASS if mismatch_count == 0 else EXIT_MISMATCH
    document = {
        "binaries": {
            "candidate": binary_identity(candidate),
            "reference": binary_identity(reference),
        },
        "cases": cases,
        "environment": manifest["environment"],
        "exit_code": exit_code,
        "manifest": {
            "path": manifest_relative.as_posix(),
            "sha256": sha256_file(manifest_path),
        },
        "normalization": {"enabled": False, "rules": []},
        "repository_commit": repository_commit(),
        "schema_version": 1,
        "status": "pass" if exit_code == EXIT_PASS else "mismatch",
        "summary": {
            "mismatch": mismatch_count,
            "pass": len(cases) - mismatch_count,
            "total": len(cases),
        },
        "wave": 4,
    }
    return document, exit_code


FAKE_PHP = r"""#!/usr/bin/env python3
import base64
import json
import os
import pathlib
import sys

def fnv1a64(filename, source):
    value = 0xcbf29ce484222325
    for byte in filename.encode("utf-8") + b"\0" + source:
        value ^= byte
        value = (value * 0x100000001b3) & 0xffffffffffffffff
    return "fnv1a64:{:016x}".format(value)

codes = {
    "try_catch_finally": "MIRL0015",
    "call_inside_branch": "MIRL0012",
    "reference_loop_phi": "MIRL0013",
    "array_condition": "MIRL0013",
    "object_condition": "MIRL0013",
    "string_condition_without_profile": "MIRL0018",
    "switch_or_match_if_not_profiled": "MIRL0001",
    "goto_irreducible": "MIRL0016",
    "unsupported_pi_constraint": "MIRL0017",
    "missing_condition_type_proof": "MIRL0018",
    "coalesce_reference_semantics": "MIRL0013",
    "nullsafe_jump": "MIRL0013",
}

if sys.argv[-1].endswith("invoke_dump.php"):
    request = json.loads(sys.stdin.buffer.read())
    assert request["options"]["wave"] == 4
    source = base64.b64decode(request["source_base64"], validate=True)
    filename = request["filename"]
    case_id = pathlib.Path(filename).stem
    rejected = "/rejected/" in filename
    mir_lines = ["znmir 1.3 module m0", "function f0 symbol s0 entry b0 flags 0x00000000"]
    for block in range(12):
        target = (block + 1) % 12
        mir_lines.append("block b{} function f0 predecessors [] successors [b{}]".format(block, target))
    for phi in range(4):
        mir_lines.append("instruction i{} block b{} opcode phi representation i64 result v{} operands []".format(phi, phi, phi))
    mir_lines.append("end")
    if rejected:
        result = {
            "diagnostics": [{"code": codes[case_id], "message": "stable rejection", "opline": 0, "stage": "MIRL"}],
            "mir": None,
            "phase": "lowering",
            "schema_version": 1,
            "source": {"byte_length": len(source), "filename": filename, "source_id": fnv1a64(filename, source)},
            "status": "rejected",
            "wave": 4,
        }
    else:
        result = {
            "diagnostics": [{"code": "MIRL0000", "message": "W04 lowering completed", "opline": None, "stage": "MIRL"}],
            "mir": "\n".join(mir_lines) + "\n",
            "phase": "complete",
            "schema_version": 1,
            "source": {"byte_length": len(source), "filename": filename, "source_id": fnv1a64(filename, source)},
            "status": "accepted",
            "wave": 4,
        }
    print(json.dumps({"calls": [result] * request["repeat"], "schema_version": 1}, sort_keys=True))
    raise SystemExit(0)

if os.environ.get("W04_FAKE_MISMATCH") == "1":
    sys.stdout.buffer.write(b"candidate-mismatch\n")
else:
    sys.stdout.buffer.write(b"stable-output\n")
"""


def self_test() -> None:
    with tempfile.TemporaryDirectory(prefix="w04-differential-selftest-") as directory:
        root = Path(directory)
        reference = root / "reference-php"
        candidate = root / "candidate php"
        reference.write_text(FAKE_PHP, encoding="utf-8")
        candidate.write_text(FAKE_PHP, encoding="utf-8")
        reference.chmod(0o755)
        candidate.chmod(0o755)

        document, exit_code = run(
            str(reference),
            str(candidate),
            DEFAULT_MANIFEST,
            SELF_TEST_TIMEOUT,
        )
        if exit_code != EXIT_PASS or document["summary"] != {
            "mismatch": 0,
            "pass": 24,
            "total": 24,
        }:
            raise DifferentialError("self-test corpus comparison did not pass")
        if any(
            [call["call"] for call in case["dump"]["calls"]] != list(range(1, 11))
            for case in document["cases"]
        ):
            raise DifferentialError("self-test call attribution is incomplete")

        source = root / "source;touch SHOULD_NOT_EXIST.php"
        source.write_text("<?php echo 'not run by dump';\n", encoding="utf-8")
        reference_run = execute_source(reference, source, SELF_TEST_TIMEOUT)
        mismatch_environment = deterministic_environment()
        mismatch_environment["W04_FAKE_MISMATCH"] = "1"
        mismatch_run = execute_source(
            candidate, source, SELF_TEST_TIMEOUT, mismatch_environment
        )
        if execution_differences(reference_run, mismatch_run) != ["stdout"]:
            raise DifferentialError("self-test did not detect byte mismatch")
        dump, _ = dump_w04.invoke(
            str(candidate),
            source.read_bytes(),
            "tests/native/control-flow/corpus/accepted/if_else_int.php",
            repeat=10,
            timeout=SELF_TEST_TIMEOUT,
        )
        if len(dump["calls"]) != 10:
            raise DifferentialError("self-test repeated dump failed")
        if (root / "SHOULD_NOT_EXIST").exists() or (ROOT / "SHOULD_NOT_EXIST").exists():
            raise DifferentialError("self-test detected shell evaluation")


def stable_json_write(document: dict[str, Any], destination: Path) -> None:
    payload = (
        json.dumps(document, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    ).encode("utf-8")
    destination = destination.resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".tmp")
    temporary.write_bytes(payload)
    os.replace(temporary, destination)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-php", help="absolute reference PHP path")
    parser.add_argument("--candidate-php", help="absolute W04 candidate PHP path")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        if args.self_test:
            if args.reference_php or args.candidate_php or args.json_out:
                raise DifferentialError("--self-test does not accept execution arguments")
            self_test()
            print("W04 differential harness self-test passed")
            return EXIT_PASS
        if (
            not args.reference_php
            or not args.candidate_php
            or args.json_out is None
        ):
            raise DifferentialError(
                "--reference-php, --candidate-php, and --json-out are required"
            )
        if args.json_out.resolve().is_relative_to(CORPUS):
            raise DifferentialError("--json-out must not overwrite corpus evidence")
        document, exit_code = run(
            args.reference_php,
            args.candidate_php,
            args.manifest,
            args.timeout,
        )
        stable_json_write(document, args.json_out)
        return exit_code
    except (
        OSError,
        DifferentialError,
        manifest_validator.ManifestError,
        dump_w04.DumpError,
    ) as error:
        if args.json_out is not None:
            stable_json_write(
                {
                    "error": str(error),
                    "schema_version": 1,
                    "status": "error",
                    "wave": 4,
                },
                args.json_out,
            )
        print("run-w04-differential.py: {}".format(error), file=sys.stderr)
        return EXIT_HARNESS_ERROR


if __name__ == "__main__":
    raise SystemExit(main())
