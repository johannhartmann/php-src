#!/usr/bin/env python3
"""Run W04 real-PHP Stage-1/2/3, determinism, and differential tests."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
MANIFEST_PATH = ROOT / "tests/native/control-flow/corpus/manifest.json"
DUMP_PATH = ROOT / "scripts/native/control-flow/dump-w04.py"
DIFFERENTIAL_PATH = ROOT / "scripts/native/control-flow/run-w04-differential.py"
GOLDEN_ROOT = ROOT / "tests/native/control-flow/integration/goldens"
GOLDEN_INDEX = GOLDEN_ROOT / "index.json"
ARENA_CHUNK_SIZES = (64, 4096, 65536)
OPCACHE_MODES = (False, True)


class W04TestError(RuntimeError):
    """A W04 control-flow or differential invariant failed."""


def load_module(name: str, path: Path) -> Any:
    specification = importlib.util.spec_from_file_location(name, path)
    if specification is None or specification.loader is None:
        raise W04TestError("unable to load {}".format(path))
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


dump_w04 = load_module("w04_integration_dump", DUMP_PATH)


def stable_write(document: Any, destination: Path) -> None:
    payload = (
        json.dumps(document, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    ).encode("utf-8")
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".tmp")
    temporary.write_bytes(payload)
    os.replace(temporary, destination)


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def canonical_binary(value: str, label: str) -> Path:
    candidate = Path(value).expanduser()
    if not candidate.is_absolute():
        raise W04TestError("{} PHP path must be absolute".format(label))
    resolved = candidate.resolve()
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise W04TestError("{} PHP is not executable: {}".format(label, value))
    return resolved


def deterministic_environment(
    reference: Path | None, candidate: Path, sanitizer: str | None
) -> dict[str, str]:
    environment = os.environ.copy()
    asan_detect_leaks = "0" if sys.platform == "darwin" else "1"
    environment.update(
        {
            "LANG": "C",
            "LC_ALL": "C",
            "TZ": "UTC",
            "SOURCE_DATE_EPOCH": "0",
            "PYTHONDONTWRITEBYTECODE": "1",
            "W04_CANDIDATE_PHP": str(candidate),
            "ASAN_OPTIONS": (
                "abort_on_error=1:detect_leaks={}".format(asan_detect_leaks)
            ),
            "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
        }
    )
    if reference is not None:
        environment["W04_REFERENCE_PHP"] = str(reference)
    if sanitizer is not None:
        environment["W04_SANITIZER"] = sanitizer
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
        raise W04TestError(
            "unable to inspect candidate PHP: exit={} stderr={}".format(
                completed.returncode, completed.stderr.strip()
            )
        )
    try:
        document = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise W04TestError("candidate PHP returned invalid capability JSON") from error
    if document.get("bridge") is not True:
        raise W04TestError("candidate PHP does not contain native_mir_test")
    if document.get("opcache") is not True:
        raise W04TestError("candidate PHP does not contain Zend OPcache")
    return document


def cfg_properties(mir: str) -> dict[str, int]:
    blocks = 0
    edges = 0
    backedges = 0
    phis = 0
    statepoints = 0
    for line in mir.splitlines():
        block = re.match(
            r"^block b([0-9]+)\b.*\bsuccessors \[([^\]]*)\]", line
        )
        if block is not None:
            source = int(block.group(1))
            successors = [
                int(value)
                for value in re.findall(r"\bb([0-9]+)\b", block.group(2))
            ]
            blocks += 1
            edges += len(successors)
            backedges += sum(target <= source for target in successors)
        if re.match(r"^instruction i[0-9]+\b.*\bopcode phi\b", line):
            phis += 1
        if re.match(r"^instruction i[0-9]+\b.*\bopcode statepoint\b", line):
            statepoints += 1
    return {
        "backedges": backedges,
        "blocks": blocks,
        "edges": edges,
        "phis": phis,
        "statepoints": statepoints,
    }


def accepted_cases() -> list[dict[str, Any]]:
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    return [
        case for case in manifest["cases"] if case["expected_status"] == "accepted"
    ]


def dump_case(
    candidate: Path,
    case: dict[str, Any],
    environment: dict[str, str],
    *,
    chunk_size: int = 4096,
    opcache_enabled: bool = False,
    repeat: int = 1,
) -> dict[str, Any]:
    source = ROOT / case["source_path"]
    document, _ = dump_w04.invoke(
        str(candidate),
        source.read_bytes(),
        case["source_path"],
        function=case["function"],
        repeat=repeat,
        arena_chunk_size=chunk_size,
        opcache_enabled=opcache_enabled,
        timeout=60.0,
        environment=environment,
    )
    return document


def write_goldens(candidate: Path, environment: dict[str, str]) -> None:
    entries: dict[str, Any] = {}
    GOLDEN_ROOT.mkdir(parents=True, exist_ok=True)
    expected_names = set()
    for case in accepted_cases():
        document = dump_case(candidate, case, environment)
        result = document["calls"][0]["result"]
        if result["status"] != "accepted" or result["mir"] is None:
            raise W04TestError(
                "{} did not produce accepted MIR".format(case["case_id"])
            )
        payload = result["mir"].encode("utf-8")
        relative_path = "tests/native/control-flow/integration/goldens/{}.znmir".format(
            case["case_id"]
        )
        destination = ROOT / relative_path
        destination.write_bytes(payload)
        expected_names.add(destination.name)
        entries[case["case_id"]] = {
            "cfg": cfg_properties(result["mir"]),
            "path": relative_path,
            "sha256": sha256_bytes(payload),
        }
    for existing in GOLDEN_ROOT.glob("*.znmir"):
        if existing.name not in expected_names:
            existing.unlink()
    stable_write(
        {"cases": entries, "format_version": 1, "normalization": False},
        GOLDEN_INDEX,
    )


def verify_goldens(candidate: Path, environment: dict[str, str]) -> dict[str, Any]:
    if not GOLDEN_INDEX.is_file():
        raise W04TestError("integration golden index is missing")
    index = json.loads(GOLDEN_INDEX.read_text(encoding="utf-8"))
    cases = {case["case_id"]: case for case in accepted_cases()}
    entries = index.get("cases")
    if not isinstance(entries, dict) or set(entries) != set(cases):
        raise W04TestError("integration golden case set differs from accepted corpus")
    coverage = {
        "backedges": 0,
        "blocks": 0,
        "edges": 0,
        "phis": 0,
        "statepoints": 0,
    }
    hashes: dict[str, str] = {}
    for case_id, case in cases.items():
        result = dump_case(candidate, case, environment)["calls"][0]["result"]
        payload = (result.get("mir") or "").encode("utf-8")
        entry = entries[case_id]
        golden = ROOT / entry["path"]
        if not golden.is_file() or golden.read_bytes() != payload:
            raise W04TestError("{} differs from its integration golden".format(case_id))
        digest = sha256_bytes(payload)
        properties = cfg_properties(result["mir"])
        if entry.get("sha256") != digest or entry.get("cfg") != properties:
            raise W04TestError("{} golden index drifted".format(case_id))
        hashes[case_id] = digest
        for key in coverage:
            coverage[key] += properties[key]
    if coverage["phis"] < 1 or coverage["backedges"] < 1 or coverage["statepoints"] < 1:
        raise W04TestError("goldens lack PHI, loop, or edge-statepoint coverage")
    return {"cfg_totals": coverage, "hashes": hashes, "status": "pass"}


def deterministic_axes(
    candidate: Path, environment: dict[str, str]
) -> dict[str, Any]:
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    observations = []
    for case in manifest["cases"]:
        baseline: dict[str, Any] | None = None
        hashes = []
        for chunk_size in ARENA_CHUNK_SIZES:
            for opcache_enabled in OPCACHE_MODES:
                document = dump_case(
                    candidate,
                    case,
                    environment,
                    chunk_size=chunk_size,
                    opcache_enabled=opcache_enabled,
                )
                result = document["calls"][0]["result"]
                if baseline is None:
                    baseline = result
                elif result != baseline:
                    raise W04TestError(
                        "{} changed across arena/OPcache axes".format(
                            case["case_id"]
                        )
                    )
                hashes.append(
                    {
                        "arena_chunk_size": chunk_size,
                        "opcache_enabled": opcache_enabled,
                        "sha256": sha256_bytes(
                            json.dumps(
                                result, sort_keys=True, separators=(",", ":")
                            ).encode("utf-8")
                        ),
                    }
                )
        observations.append({"case_id": case["case_id"], "observations": hashes})
    return {
        "arena_chunk_sizes": list(ARENA_CHUNK_SIZES),
        "cases": observations,
        "opcache_modes": ["disabled", "enabled"],
        "registry_order": "covered-by-unit-suite",
        "repeat_calls": list(range(1, 11)),
        "status": "pass",
    }


def run_static_suites(environment: dict[str, str]) -> None:
    run(["python3", "tests/native/control-flow/unit/run_control_flow_tests.py"], environment)
    run(
        [
            "python3",
            "-m",
            "unittest",
            "discover",
            "-s",
            "tests/native/control-flow",
            "-p",
            "test_*.py",
            "-v",
        ],
        environment,
        timeout=600,
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
            "--reference-php",
            str(reference),
            "--candidate-php",
            str(candidate),
            "--json-out",
            str(output),
        ],
        environment,
        timeout=900,
    )
    document = json.loads(output.read_text(encoding="utf-8"))
    if document.get("status") != "pass":
        raise W04TestError("W04 differential result is not pass")
    return document


def self_test() -> None:
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    run(
        [sys.executable, str(DIFFERENTIAL_PATH.relative_to(ROOT)), "--self-test"],
        environment,
        timeout=60,
    )
    if not GOLDEN_INDEX.is_file():
        raise W04TestError("integration golden index is missing")
    index = json.loads(GOLDEN_INDEX.read_text(encoding="utf-8"))
    if index.get("format_version") != 1 or index.get("normalization") is not False:
        raise W04TestError("integration golden index contract drifted")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-php")
    parser.add_argument("--candidate-php")
    parser.add_argument("--sanitizer", choices=("address", "undefined"))
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--write-goldens", action="store_true")
    arguments = parser.parse_args()
    try:
        if arguments.self_test:
            if any(
                value is not None
                for value in (
                    arguments.reference_php,
                    arguments.candidate_php,
                    arguments.sanitizer,
                    arguments.json_out,
                )
            ) or arguments.write_goldens:
                raise W04TestError("--self-test accepts no other arguments")
            self_test()
            print("W04 control-flow harness self-test passed")
            return 0
        if arguments.candidate_php is None:
            raise W04TestError("--candidate-php is required")
        candidate = canonical_binary(arguments.candidate_php, "candidate")
        reference = (
            canonical_binary(arguments.reference_php, "reference")
            if arguments.reference_php is not None
            else None
        )
        environment = deterministic_environment(
            reference, candidate, arguments.sanitizer
        )
        if arguments.write_goldens:
            if reference is not None or arguments.sanitizer is not None:
                raise W04TestError(
                    "--write-goldens accepts only --candidate-php"
                )
            write_goldens(candidate, environment)
            print("W04 integration goldens written")
            return 0
        if reference is None:
            raise W04TestError("--reference-php is required")
        if os.path.samefile(reference, candidate):
            raise W04TestError("reference and candidate PHP must be distinct")
        capabilities = php_capabilities(candidate, environment)
        with tempfile.TemporaryDirectory(prefix="w04-integration-") as directory:
            differential_path = Path(directory) / "differential.json"
            if arguments.sanitizer is None:
                run_static_suites(environment)
            differential = run_differential(
                reference, candidate, environment, differential_path
            )
            determinism = deterministic_axes(candidate, environment)
            goldens = verify_goldens(candidate, environment)
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
            "determinism": determinism,
            "differential_summary": differential["summary"],
            "goldens": goldens,
            "sanitizer": arguments.sanitizer or "none",
            "schema_version": 1,
            "status": "pass",
        }
        if arguments.json_out is not None:
            stable_write(result, arguments.json_out)
        print(
            "W04 real-PHP integration tests passed "
            "(sanitizer={}, cases={})".format(
                arguments.sanitizer or "none",
                differential["summary"]["total"],
            )
        )
        return 0
    except (
        OSError,
        subprocess.CalledProcessError,
        subprocess.TimeoutExpired,
        W04TestError,
        dump_w04.DumpError,
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
        print("test-w04.py: {}".format(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
