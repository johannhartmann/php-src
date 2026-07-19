#!/usr/bin/env python3
"""Run the real-PHP W03 lowering, determinism, and differential gate."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
MANIFEST_PATH = ROOT / "tests/native/lowering/corpus/manifest.json"
DUMP_PATH = ROOT / "scripts/native/lowering/dump-mir.py"
DIFFERENTIAL_PATH = ROOT / "scripts/native/lowering/run-w03-differential.py"
SPECIALIST_RUNNERS = (
    "tests/native/lowering/core/run_core_lowering_tests.py",
    "tests/native/lowering/frontend/run_frontend_tests.py",
    "tests/native/lowering/numeric/run_numeric_tests.py",
    "tests/native/lowering/logic/run_logic_tests.py",
    "tests/native/lowering/lifetime/run_lifetime_tests.py",
    "tests/native/lowering/mir/run_scalar_mir_tests.py",
)
PYTHON_TEST_DIRS = (
    "tests/native/lowering/core",
    "tests/native/lowering/frontend",
    "tests/native/lowering/numeric",
    "tests/native/lowering/logic",
    "tests/native/lowering/lifetime",
    "tests/native/lowering/mir",
    "tests/native/lowering/bridge",
    "tests/native/lowering/integration",
)
ARENA_CHUNK_SIZES = (64, 4096, 65536)
OPCACHE_MODES = (False, True)


class W03TestError(RuntimeError):
    """The W03 hard gate cannot produce passing evidence."""


def load_module(name: str, path: Path) -> Any:
    specification = importlib.util.spec_from_file_location(name, path)
    if specification is None or specification.loader is None:
        raise W03TestError(f"unable to load {path}")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


dump_mir = load_module("w03_gate_dump_mir", DUMP_PATH)


def canonical_binary(value: str, label: str) -> Path:
    candidate = Path(value).expanduser()
    if not candidate.is_absolute():
        raise W03TestError(f"{label} PHP path must be absolute")
    resolved = candidate.resolve()
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise W03TestError(f"{label} PHP is not executable: {value}")
    return resolved


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def deterministic_environment(
    reference: Path, candidate: Path, sanitizer: str | None
) -> dict[str, str]:
    environment = os.environ.copy()
    environment.update(
        {
            "LANG": "C",
            "LC_ALL": "C",
            "TZ": "UTC",
            "SOURCE_DATE_EPOCH": "0",
            "PYTHONDONTWRITEBYTECODE": "1",
            "W03_REFERENCE_PHP": str(reference),
            "W03_D_REFERENCE_PHP": str(reference),
            "W03_CANDIDATE_PHP": str(candidate),
            "ASAN_OPTIONS": "abort_on_error=1:detect_leaks=1",
            "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
        }
    )
    if sanitizer is not None:
        environment["W03_SANITIZER"] = sanitizer
    return environment


def run(
    command: list[str],
    environment: dict[str, str],
    *,
    timeout: int = 300,
) -> None:
    print("+ " + " ".join(shlex.quote(part) for part in command), flush=True)
    subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        check=True,
        timeout=timeout,
    )


def php_capabilities(binary: Path, environment: dict[str, str]) -> dict[str, Any]:
    source = (
        'echo json_encode(["debug" => (bool) PHP_DEBUG, '
        '"zts" => (bool) PHP_ZTS, '
        '"opcache" => extension_loaded("Zend OPcache"), '
        '"bridge" => extension_loaded("native_mir_test")]);'
    )
    completed = subprocess.run(
        [str(binary), "-n", "-r", source],
        cwd=ROOT,
        env=environment,
        check=False,
        capture_output=True,
        text=True,
        timeout=30,
    )
    if completed.returncode != 0 or completed.stderr:
        raise W03TestError(
            f"unable to inspect candidate PHP: exit={completed.returncode} "
            f"stderr={completed.stderr.strip()}"
        )
    try:
        document = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise W03TestError("candidate PHP returned invalid capability JSON") from error
    if document.get("bridge") is not True:
        raise W03TestError("candidate PHP does not contain native_mir_test")
    if document.get("opcache") is not True:
        raise W03TestError("candidate PHP does not contain Zend OPcache")
    return document


def run_static_and_unit_suites(environment: dict[str, str]) -> None:
    for runner in SPECIALIST_RUNNERS:
        run(["python3", runner], environment)
    for directory in PYTHON_TEST_DIRS:
        run(
            [
                "python3",
                "-m",
                "unittest",
                "discover",
                "-s",
                directory,
                "-p",
                "test_*.py",
                "-v",
            ],
            environment,
        )


def run_differential(
    reference: Path,
    candidate: Path,
    environment: dict[str, str],
    output: Path,
) -> dict[str, Any]:
    run(
        [
            "python3",
            str(DIFFERENTIAL_PATH.relative_to(ROOT)),
            "--reference",
            str(reference),
            "--candidate",
            str(candidate),
            "--json-out",
            str(output),
        ],
        environment,
        timeout=600,
    )
    document = json.loads(output.read_text(encoding="utf-8"))
    if document.get("status") != "pass":
        raise W03TestError("W03 differential result is not pass")
    return document


def deterministic_axes(
    candidate: Path, environment: dict[str, str]
) -> dict[str, Any]:
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    cases: list[dict[str, Any]] = []
    for case in manifest["cases"]:
        source = ROOT / case["source_path"]
        source_bytes = source.read_bytes()
        baseline: dict[str, Any] | None = None
        observations = []
        for chunk_size in ARENA_CHUNK_SIZES:
            for opcache_enabled in OPCACHE_MODES:
                document, _ = dump_mir.invoke(
                    str(candidate),
                    source_bytes,
                    case["source_path"],
                    function=case["function"],
                    arena_chunk_size=chunk_size,
                    opcache_enabled=opcache_enabled,
                    timeout=60.0,
                    environment=environment,
                )
                result = document["calls"][0]["result"]
                if baseline is None:
                    baseline = result
                elif result != baseline:
                    raise W03TestError(
                        "{} changed across arena/OPcache axes".format(
                            case["case_id"]
                        )
                    )
                observations.append(
                    {
                        "arena_chunk_size": chunk_size,
                        "mir_sha256": document["calls"][0]["mir_sha256"],
                        "opcache_enabled": opcache_enabled,
                    }
                )
        assert baseline is not None
        cases.append(
            {
                "case_id": case["case_id"],
                "observations": observations,
                "status": baseline["status"],
            }
        )
    return {
        "arena_chunk_sizes": list(ARENA_CHUNK_SIZES),
        "cases": cases,
        "opcache_modes": ["disabled", "enabled"],
        "status": "pass",
    }


def stable_write(document: dict[str, Any], destination: Path) -> None:
    payload = (
        json.dumps(document, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    ).encode("utf-8")
    destination = destination.resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".tmp")
    temporary.write_bytes(payload)
    os.replace(temporary, destination)


def self_test() -> None:
    environment = os.environ.copy()
    environment.update(
        {
            "LANG": "C",
            "LC_ALL": "C",
            "TZ": "UTC",
            "SOURCE_DATE_EPOCH": "0",
            "PYTHONDONTWRITEBYTECODE": "1",
        }
    )
    run(
        [
            sys.executable,
            str(DIFFERENTIAL_PATH.relative_to(ROOT)),
            "--self-test",
        ],
        environment,
        timeout=60,
    )
    with tempfile.TemporaryDirectory(prefix="w03-gate-selftest-") as directory:
        output = Path(directory) / "result.json"
        document = {
            "axes": list(ARENA_CHUNK_SIZES),
            "modes": list(OPCACHE_MODES),
            "schema_version": 1,
            "status": "pass",
        }
        stable_write(document, output)
        first = output.read_bytes()
        stable_write(document, output)
        if output.read_bytes() != first:
            raise W03TestError("stable result writer is nondeterministic")
        if output.with_name(output.name + ".tmp").exists():
            raise W03TestError("stable result writer left a temporary file")
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    cases = manifest.get("cases")
    if not isinstance(cases, list) or not cases:
        raise W03TestError("W03 corpus manifest has no cases")
    case_ids = [case.get("case_id") for case in cases]
    if any(not isinstance(case_id, str) or not case_id for case_id in case_ids):
        raise W03TestError("W03 corpus contains an invalid case ID")
    if len(case_ids) != len(set(case_ids)):
        raise W03TestError("W03 corpus contains duplicate case IDs")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-php")
    parser.add_argument("--candidate-php")
    parser.add_argument(
        "--sanitizer", choices=("address", "undefined"), default=None
    )
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--self-test", action="store_true")
    arguments = parser.parse_args()
    try:
        if arguments.self_test:
            if (
                arguments.reference_php is not None
                or arguments.candidate_php is not None
                or arguments.sanitizer is not None
                or arguments.json_out is not None
            ):
                raise W03TestError(
                    "--self-test does not accept PHP, sanitizer, or output arguments"
                )
            self_test()
            print("W03 hard-gate harness self-test passed")
            return 0
        if arguments.reference_php is None or arguments.candidate_php is None:
            raise W03TestError(
                "--reference-php and --candidate-php are required"
            )
        reference = canonical_binary(arguments.reference_php, "reference")
        candidate = canonical_binary(arguments.candidate_php, "candidate")
        if os.path.samefile(reference, candidate):
            raise W03TestError("reference and candidate PHP must be distinct")
        environment = deterministic_environment(
            reference, candidate, arguments.sanitizer
        )
        capabilities = php_capabilities(candidate, environment)
        with tempfile.TemporaryDirectory(prefix="w03-hard-gate-") as directory:
            differential_path = Path(directory) / "differential.json"
            if arguments.sanitizer is None:
                run_static_and_unit_suites(environment)
            differential = run_differential(
                reference, candidate, environment, differential_path
            )
            axes = deterministic_axes(candidate, environment)
        result = {
            "binaries": {
                "candidate": {
                    "capabilities": capabilities,
                    "path": str(candidate),
                    "sha256": sha256_file(candidate),
                },
                "reference": {
                    "path": str(reference),
                    "sha256": sha256_file(reference),
                },
            },
            "determinism": axes,
            "differential_summary": differential["summary"],
            "sanitizer": arguments.sanitizer or "none",
            "schema_version": 1,
            "status": "pass",
        }
        if arguments.json_out is not None:
            stable_write(result, arguments.json_out)
        print(
            "W03 real-PHP hard gate passed "
            f"(sanitizer={arguments.sanitizer or 'none'}, "
            f"cases={differential['summary']['total']})"
        )
        return 0
    except (
        OSError,
        subprocess.CalledProcessError,
        subprocess.TimeoutExpired,
        W03TestError,
        dump_mir.DumpError,
        json.JSONDecodeError,
    ) as error:
        if arguments.json_out is not None:
            stable_write(
                {
                    "error": str(error),
                    "sanitizer": arguments.sanitizer or "none",
                    "schema_version": 1,
                    "status": "fail",
                },
                arguments.json_out,
            )
        print(f"test-w03.py: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
