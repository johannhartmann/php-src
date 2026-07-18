#!/usr/bin/env python3
"""Run byte-exact PHP execution and separate W03 MIR dump comparisons."""

from __future__ import annotations

import argparse
import base64
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
CORPUS = ROOT / "tests/native/lowering/corpus"
DEFAULT_MANIFEST = CORPUS / "manifest.json"
VALIDATOR_PATH = CORPUS / "validate_manifest.py"
DUMP_PATH = ROOT / "scripts/native/lowering/dump-mir.py"

EXIT_PASS = 0
EXIT_MISMATCH = 1
EXIT_HARNESS_ERROR = 2


class DifferentialError(ValueError):
    """The differential harness cannot produce trustworthy evidence."""


def load_module(name: str, path: Path) -> Any:
    specification = importlib.util.spec_from_file_location(name, path)
    if specification is None or specification.loader is None:
        raise DifferentialError("unable to load {}".format(path))
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


dump_mir = load_module("w03_dump_mir", DUMP_PATH)
manifest_validator = load_module("w03_manifest_validator", VALIDATOR_PATH)


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


def deterministic_environment() -> dict[str, str]:
    environment = os.environ.copy()
    environment.update(
        {
            "LANG": "C",
            "LC_ALL": "C",
            "TZ": "UTC",
            "SOURCE_DATE_EPOCH": "0",
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


def execute_source(binary: Path, source: Path, timeout: float) -> subprocess.CompletedProcess[bytes]:
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
            env=deterministic_environment(),
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


def check_dump(
    case: dict[str, Any],
    candidate: Path,
    source: Path,
    timeout: float,
) -> tuple[dict[str, Any], list[str]]:
    repeat = max(case["repeat_calls"])
    document, _ = dump_mir.invoke(
        str(candidate),
        source.read_bytes(),
        case["source_path"],
        function=case["function"],
        repeat=repeat,
        timeout=timeout,
    )
    differences = []
    expected_status = case["disposition"]
    if document["status"] != expected_status:
        differences.append("dump_status")
    selected_calls = []
    for call_number in case["repeat_calls"]:
        call = document["calls"][call_number - 1]
        selected_calls.append(call)
        result = call["result"]
        if case["expected_mirl"] not in diagnostic_codes(result):
            differences.append("diagnostic_call_{}".format(call_number))
        if case["disposition"] == "accepted":
            golden = ROOT / case["golden_path"]
            golden_bytes = golden.read_bytes()
            if result["mir"].encode("utf-8") != golden_bytes:
                differences.append("golden_call_{}".format(call_number))
            if call["mir_sha256"] != case["golden_sha256"]:
                differences.append("golden_hash_call_{}".format(call_number))
        elif result["mir"] is not None:
            differences.append("partial_mir_call_{}".format(call_number))
    return (
        {
            "calls": selected_calls,
            "normalization": document["normalization"],
            "status": document["status"],
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
    }
    return document, exit_code


FAKE_PHP = r"""#!/usr/bin/env python3
import base64
import hashlib
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

if sys.argv[-1].endswith("invoke_dump.php"):
    request = json.loads(sys.stdin.buffer.read())
    source = base64.b64decode(request["source_base64"], validate=True)
    filename = request["filename"]
    mir = "znmir 1.2\nmodule m0\nsource " + hashlib.sha256(source).hexdigest() + "\n"
    result = {
        "diagnostics": [{"code": "MIRL0000", "message": "ok", "opline": 0, "stage": "MIRL"}],
        "mir": mir,
        "phase": "complete",
        "schema_version": 1,
        "source": {
            "byte_length": len(source),
            "filename": filename,
            "source_id": fnv1a64(filename, source),
        },
        "status": "accepted",
    }
    print(json.dumps({"calls": [result] * request["repeat"], "schema_version": 1}, sort_keys=True))
    raise SystemExit(0)

source = pathlib.Path(sys.argv[-1]).read_bytes()
sys.stdout.buffer.write(source if os.environ.get("W03_FAKE_MISMATCH") == "1" else b"stable-output\n")
"""


def self_test() -> None:
    with tempfile.TemporaryDirectory(prefix="w03-differential-selftest-") as directory:
        root = Path(directory)
        reference = root / "reference-php"
        candidate = root / "candidate php"
        reference.write_text(FAKE_PHP, encoding="utf-8")
        candidate.write_text(FAKE_PHP, encoding="utf-8")
        reference.chmod(0o755)
        candidate.chmod(0o755)
        source = root / "source;touch SHOULD_NOT_EXIST.php"
        source.write_text("<?php return 1;\n", encoding="utf-8")

        reference_run = execute_source(reference, source, 5.0)
        candidate_run = execute_source(candidate, source, 5.0)
        if execution_differences(reference_run, candidate_run):
            raise DifferentialError("self-test equivalent execution did not compare equal")
        mismatching_environment = deterministic_environment()
        mismatching_environment["W03_FAKE_MISMATCH"] = "1"
        mismatching_run = subprocess.run(
            [
                str(candidate),
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
            timeout=5.0,
            env=mismatching_environment,
            cwd=source.parent,
        )
        if execution_differences(reference_run, mismatching_run) != ["stdout"]:
            raise DifferentialError("self-test did not detect an execution mismatch")

        dump, exit_code = dump_mir.invoke(
            str(candidate),
            source.read_bytes(),
            "source;touch SHOULD_NOT_EXIST.php",
            repeat=10,
            timeout=5.0,
        )
        if exit_code != dump_mir.EXIT_ACCEPTED or len(dump["calls"]) != 10:
            raise DifferentialError("self-test repeated accepted dump failed")
        if (ROOT / "SHOULD_NOT_EXIST").exists() or (root / "SHOULD_NOT_EXIST").exists():
            raise DifferentialError("self-test detected shell evaluation")

        expected_mir = dump["calls"][0]["result"]["mir"].encode("utf-8")
        golden = root / "golden.znmir"
        golden.write_bytes(expected_mir)
        case = {
            "case_id": "selftest",
            "deferred_wave": None,
            "disposition": "accepted",
            "expected_mirl": "MIRL0000",
            "family": "constants",
            "function": None,
            "golden_path": str(golden),
            "golden_sha256": hashlib.sha256(expected_mir).hexdigest(),
            "reference_path": str(source),
            "repeat_calls": list(range(1, 11)),
            "source_path": str(source),
            "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        }
        _, differences = check_dump(case, candidate, source, 5.0)
        if differences:
            raise DifferentialError(
                "self-test golden comparison failed: {}".format(
                    ", ".join(differences)
                )
            )
        golden.write_text("znmir corrupted\n", encoding="utf-8")
        _, differences = check_dump(case, candidate, source, 5.0)
        expected_differences = {
            "golden_call_{}".format(call) for call in range(1, 11)
        }
        if not expected_differences.issubset(differences):
            raise DifferentialError("self-test did not detect a golden mismatch")
        if case["repeat_calls"] != list(range(1, 11)):
            raise DifferentialError("self-test call attribution is incomplete")


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
    parser.add_argument("--reference", help="absolute reference PHP path")
    parser.add_argument("--candidate", help="absolute W03 candidate PHP path")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        if args.self_test:
            if args.reference or args.candidate or args.json_out:
                raise DifferentialError("--self-test does not accept execution arguments")
            self_test()
            print("W03 differential harness self-test passed")
            return EXIT_PASS
        if not args.reference or not args.candidate or args.json_out is None:
            raise DifferentialError(
                "--reference, --candidate, and --json-out are required"
            )
        if args.json_out.resolve().is_relative_to(CORPUS):
            raise DifferentialError("--json-out must not overwrite corpus oracles")
        document, exit_code = run(
            args.reference,
            args.candidate,
            args.manifest,
            args.timeout,
        )
        stable_json_write(document, args.json_out)
        return exit_code
    except (OSError, DifferentialError, manifest_validator.ManifestError, dump_mir.DumpError) as error:
        if args.json_out is not None:
            stable_json_write(
                {"error": str(error), "schema_version": 1, "status": "error"},
                args.json_out,
            )
        print("run-w03-differential.py: {}".format(error), file=sys.stderr)
        return EXIT_HARNESS_ERROR


if __name__ == "__main__":
    raise SystemExit(main())
