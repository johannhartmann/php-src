#!/usr/bin/env python3
"""Validate W02 and maintain its timestamp-free coverage report."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[3]
MIR = ROOT / "Zend/Native/MIR"
REPORT = ROOT / "docs/native-engine/mir/w02-coverage-report.json"
POSITIVE_PROGRAMS = (
    "linear-values",
    "diamond-with-phi",
    "loop-with-backedge-phi",
    "critical-edge-split",
    "ownership-move-destroy",
    "canonicalize-before-phi",
    "exception-statepoint",
    "bailout-terminal",
    "nested-frame-state",
    "generator-suspend-metadata",
    "fiber-switch-metadata",
)
NEGATIVE_PROGRAMS = {
    "invalid-double-destroy": "MIRV0404",
    "invalid-missing-frame-state": "MIRV0507",
    "invalid-phi-slot": "MIRV0208",
    "invalid-unknown-op": "MIRV0102",
    "invalid-use-before-def": "MIRV0300",
}
COMMANDS = (
    "python3 scripts/native/semantics/validate-w01.py --check",
    "python3 scripts/native/mir/check-contract.py --check",
    "python3 scripts/native/mir/generate-semantic-ids.py --check",
    "python3 scripts/native/mir/test-w02.py --cc ${CC:-cc}",
    "python3 scripts/native/mir/test-w02.py --cc ${CC:-cc} --sanitizer address",
    "python3 scripts/native/mir/test-w02.py --cc ${CC:-cc} --sanitizer undefined",
    "python3 tests/native/mir/fuzz/run_mutation_fuzz.py --seed 20260717 --cases 20000",
    "python3 -m unittest discover -s tests/native/mir -p test_*.py -v",
    "scripts/native/build.sh --profile debug-nts",
    "scripts/native/test-smoke.sh --profile debug-nts",
    "scripts/native/build.sh --profile debug-zts",
    "scripts/native/test-smoke.sh --profile debug-zts",
)
FORBIDDEN_INCLUDE = re.compile(
    r"^\s*#\s*include\s*[<\"][^>\"]*"
    r"(?:tpde|dynasm|x86|amd64|aarch64|arm64|riscv|"
    r"elf|macho|coff|zend_jit|zend_vm)[^>\"]*[>\"]",
    re.IGNORECASE | re.MULTILINE,
)
FORBIDDEN_TOKEN = re.compile(r"\b(?:TPDE|DynASM)\b", re.IGNORECASE)


class W02Error(RuntimeError):
    """A deterministic W02 validation failure."""


def production_sources() -> tuple[Path, ...]:
    return tuple(sorted(MIR.rglob("*.c")))


def production_headers() -> tuple[Path, ...]:
    return tuple(sorted(MIR.rglob("*.h")))


def relative(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def tree_digest(paths: tuple[Path, ...]) -> str:
    digest = hashlib.sha256()
    for path in paths:
        digest.update(relative(path).encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def golden_digests() -> dict[str, str]:
    sums = ROOT / "tests/native/mir/golden/SHA256SUMS"
    result: dict[str, str] = {}
    for line in sums.read_text(encoding="ascii").splitlines():
        digest, filename = line.split("  ", 1)
        result[filename] = digest
    return dict(sorted(result.items()))


def architecture_leaks() -> list[str]:
    leaks: list[str] = []
    for path in (*production_sources(), *production_headers()):
        text = path.read_text(encoding="utf-8")
        include = FORBIDDEN_INCLUDE.search(text)
        token = FORBIDDEN_TOKEN.search(text)
        if include is not None:
            leaks.append(f"{relative(path)}: forbidden include {include.group(0).strip()}")
        elif token is not None:
            leaks.append(f"{relative(path)}: forbidden token {token.group(0)}")
    return leaks


def report_bytes() -> bytes:
    sources = production_sources()
    headers = production_headers()
    report = {
        "architecture_independence": {
            "forbidden_include_families": [
                "DynASM",
                "TPDE",
                "architecture",
                "object-format",
                "register",
                "runtime-VM",
            ],
            "leak_count": len(architecture_leaks()),
            "status": "pass" if not architecture_leaks() else "fail",
        },
        "contract": {
            "format": "znmir-text-v1",
            "mir_version": "1.0",
            "product_runtime_activation": False,
            "target_neutral": True,
            "wave": "W02",
        },
        "determinism": {
            "axes": [
                "allocator-chunk-size",
                "record-insertion-order",
                "repeat-execution",
                "writer-chunk-size",
            ],
            "golden_sha256": golden_digests(),
            "report_has_timestamp": False,
        },
        "gates": list(COMMANDS),
        "integration_programs": {
            "negative": [
                {"diagnostic": code, "name": name}
                for name, code in sorted(NEGATIVE_PROGRAMS.items())
            ],
            "positive": list(POSITIVE_PROGRAMS),
            "verifier_nonmutation": "bytewise fixture snapshots before and after",
        },
        "production_inventory": {
            "combined_sha256": tree_digest((*sources, *headers)),
            "header_count": len(headers),
            "source_count": len(sources),
            "sources": [relative(path) for path in sources],
        },
        "schema_version": 1,
    }
    return (json.dumps(report, indent=2, sort_keys=True) + "\n").encode("utf-8")


def run(command: list[str], environment: dict[str, str]) -> None:
    print("+ " + " ".join(shlex.quote(part) for part in command), flush=True)
    subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        check=True,
        timeout=60,
    )


def validate_report() -> None:
    expected = report_bytes()
    if not REPORT.exists():
        raise W02Error(f"missing deterministic report: {relative(REPORT)}")
    actual = REPORT.read_bytes()
    if actual != expected:
        raise W02Error(
            "W02 coverage report is stale; run "
            "python3 scripts/native/mir/validate-w02.py --write-report"
        )
    if expected != report_bytes():
        raise W02Error("W02 report generation is nondeterministic")


def validate() -> None:
    leaks = architecture_leaks()
    if leaks:
        raise W02Error("\n".join(leaks))
    environment = os.environ.copy()
    environment.update(
        {
            "LC_ALL": "C",
            "TZ": "UTC",
            "PYTHONDONTWRITEBYTECODE": "1",
        }
    )
    cc = os.environ.get("CC", "cc")
    run(["python3", "scripts/native/semantics/validate-w01.py", "--check"], environment)
    run(["python3", "scripts/native/mir/check-contract.py", "--check"], environment)
    run(
        ["python3", "scripts/native/mir/generate-semantic-ids.py", "--check"],
        environment,
    )
    run(["python3", "scripts/native/mir/test-w02.py", "--cc", cc], environment)
    run(
        [
            "python3",
            "-m",
            "unittest",
            "discover",
            "-s",
            "tests/native/mir",
            "-p",
            "test_*.py",
            "-v",
        ],
        environment,
    )
    validate_report()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--write-report", action="store_true")
    arguments = parser.parse_args()
    try:
        if arguments.write_report:
            REPORT.parent.mkdir(parents=True, exist_ok=True)
            REPORT.write_bytes(report_bytes())
            print(f"wrote {relative(REPORT)}")
        else:
            validate()
            print("W02 validation passed")
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired, W02Error) as error:
        print(f"W02 validation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
