#!/usr/bin/env python3
"""Validate W05 dumps separately from byte-exact PHP execution."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
DUMP_PATH = ROOT / "scripts/native/calls/dump-w05.py"
MANIFEST_PATH = ROOT / "tests/native/calls/corpus/manifest.json"


def load_dump_module() -> Any:
    specification = importlib.util.spec_from_file_location("dump_w05", DUMP_PATH)
    if specification is None or specification.loader is None:
        raise RuntimeError("unable to load dump-w05.py")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def diagnostic_codes(result: dict[str, Any]) -> list[str]:
    return [entry["code"] for entry in result.get("diagnostics", [])]


def call_properties(mir: str) -> dict[str, int]:
    lines = mir.splitlines()
    return {
        "arguments": sum(line.startswith("call-argument ") for line in lines),
        "continuations": sum(
            line.startswith("call-continuation ") for line in lines
        ),
        "sites": sum(line.startswith("call-site ") for line in lines),
        "targets": sum(line.startswith("call-target ") for line in lines),
    }


def self_test() -> None:
    modeled = {
        "status": "accepted",
        "diagnostics": [{"code": "MIRL0000"}],
        "mir": "\n".join(
            [
                "instruction i0 block b0 opcode call_direct_user",
                "call-target ct0",
                "call-continuation cc0",
                "call-continuation cc1",
                "call-continuation cc2",
                "call-continuation cc3",
                "call-site cs0",
            ]
        ),
    }
    assert diagnostic_codes(modeled) == ["MIRL0000"]
    assert call_properties(modeled["mir"]) == {
        "arguments": 0,
        "continuations": 4,
        "sites": 1,
        "targets": 1,
    }
    rejected = {"status": "rejected", "diagnostics": [{"code": "MIRL0023"}]}
    assert "MIRL0023" in diagnostic_codes(rejected)


def execute(binary: Path, source: Path, timeout: float) -> bytes:
    environment = os.environ.copy()
    environment.update(
        {"LANG": "C", "LC_ALL": "C", "TZ": "UTC", "SOURCE_DATE_EPOCH": "0"}
    )
    completed = subprocess.run(
        [str(binary), "-n", str(source)],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
        timeout=timeout,
        env=environment,
    )
    return str(completed.returncode).encode() + b"\0" + completed.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--reference-php")
    parser.add_argument("--candidate-php")
    parser.add_argument("--timeout", type=float, default=20.0)
    arguments = parser.parse_args()
    if arguments.self_test:
        self_test()
        print("W05 differential self-test: PASS")
        return 0
    if not arguments.reference_php or not arguments.candidate_php:
        parser.error("--reference-php and --candidate-php are required")
    dump_w05 = load_dump_module()
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    reference = Path(arguments.reference_php).resolve()
    candidate = dump_w05.candidate_path(arguments.candidate_php)
    failures: list[str] = []
    for case in manifest["cases"]:
        source = ROOT / case["source"]
        if execute(reference, source, arguments.timeout) != execute(
            candidate, source, arguments.timeout
        ):
            failures.append(f"{case['id']}:execution")
        document = dump_w05.invoke(
            candidate,
            source.read_bytes(),
            case["source"],
            case["function"],
            repeat=2,
            timeout=arguments.timeout,
            compiler_mode=case.get("compiler_mode"),
        )
        first, second = document["calls"]
        if first != second:
            failures.append(f"{case['id']}:nondeterministic")
        if first["status"] != case["status"]:
            failures.append(f"{case['id']}:status")
        if case.get("mirl") not in diagnostic_codes(first):
            failures.append(f"{case['id']}:diagnostic")
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("W05 differential: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
