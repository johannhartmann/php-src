#!/usr/bin/env python3
"""Run a wave revalidation suite with one immutable log per command."""

import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys
import time
from typing import Any, Dict, List, Optional, Sequence, Tuple


REPO_ROOT = Path(__file__).resolve().parents[2]


def w04_commands(args: argparse.Namespace) -> List[Tuple[str, List[str]]]:
    python = sys.executable
    return [
        ("validate-w01", [python, "scripts/native/semantics/validate-w01.py", "--check"]),
        ("validate-w02", [python, "scripts/native/mir/validate-w02.py", "--check"]),
        ("validate-w03", [python, "scripts/native/lowering/validate-w03.py", "--check"]),
        ("generate-w04-profile", [python, "scripts/native/control-flow/generate-w04-profile.py", "--check"]),
        ("check-w04-contract", [python, "scripts/native/control-flow/check-contract.py", "--check"]),
        ("validate-w04", [python, "scripts/native/control-flow/validate-w04.py", "--check"]),
        (
            "w04-tests",
            [python, "-m", "unittest", "discover", "-s", "tests/native/control-flow", "-p", "test_*.py", "-v"],
        ),
        (
            "w04-candidate",
            [
                python, "scripts/native/control-flow/test-w04.py",
                "--reference-php", str(args.reference_php),
                "--candidate-php", str(args.candidate_php),
            ],
        ),
        (
            "w04-zts",
            [
                python, "scripts/native/control-flow/test-w04.py",
                "--reference-php", str(args.reference_php),
                "--candidate-php", str(args.zts_php),
            ],
        ),
        (
            "w04-asan",
            [
                python, "scripts/native/control-flow/test-w04.py",
                "--reference-php", str(args.reference_php),
                "--candidate-php", str(args.asan_php),
                "--sanitizer", "address",
            ],
        ),
        (
            "w04-ubsan",
            [
                python, "scripts/native/control-flow/test-w04.py",
                "--reference-php", str(args.reference_php),
                "--candidate-php", str(args.ubsan_php),
                "--sanitizer", "undefined",
            ],
        ),
        (
            "w04-fuzz-20000",
            [
                python, "tests/native/control-flow/fuzz/run_fuzz.py",
                "--seed", "20260719", "--cases", "20000",
            ],
        ),
        ("build-debug-nts", ["scripts/native/build.sh", "--profile", "debug-nts"]),
        ("smoke-debug-nts", ["scripts/native/test-smoke.sh", "--profile", "debug-nts"]),
        ("build-debug-zts", ["scripts/native/build.sh", "--profile", "debug-zts"]),
        ("smoke-debug-zts", ["scripts/native/test-smoke.sh", "--profile", "debug-zts"]),
    ]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            block = handle.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def command_text(arguments: Sequence[str]) -> str:
    return " ".join(shlex.quote(argument) for argument in arguments)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wave", choices=["W04"], required=True)
    parser.add_argument("--artifact-dir", type=Path, required=True)
    parser.add_argument("--reference-php", type=Path, required=True)
    parser.add_argument("--candidate-php", type=Path, required=True)
    parser.add_argument("--zts-php", type=Path, required=True)
    parser.add_argument("--asan-php", type=Path, required=True)
    parser.add_argument("--ubsan-php", type=Path, required=True)
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    for name in ("reference_php", "candidate_php", "zts_php", "asan_php", "ubsan_php"):
        path = getattr(args, name)
        if not path.is_file():
            print("%s does not exist: %s" % (name.replace("_", "-"), path), file=sys.stderr)
            return 2
    artifact_dir = args.artifact_dir.resolve()
    artifact_dir.mkdir(parents=True, exist_ok=True)
    records: List[Dict[str, Any]] = []
    for command_id, arguments in w04_commands(args):
        log_path = artifact_dir / ("%s.log" % command_id)
        started = time.monotonic()
        with log_path.open("wb") as log:
            process = subprocess.run(
                arguments,
                cwd=REPO_ROOT,
                stdout=log,
                stderr=subprocess.STDOUT,
                check=False,
            )
        duration_ms = int((time.monotonic() - started) * 1000)
        record = {
            "artifact_id": "%s-log" % command_id,
            "command": command_text(arguments),
            "command_id": command_id,
            "duration_ms": duration_ms,
            "exit_code": process.returncode,
            "path": log_path.name,
            "sha256": sha256(log_path),
            "size_bytes": log_path.stat().st_size,
        }
        records.append(record)
        print("%s: exit %d (%d ms)" % (command_id, process.returncode, duration_ms))
        if process.returncode != 0:
            break
    manifest = {
        "created_at": dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "format_version": "1.0.0",
        "records": records,
        "wave_id": args.wave,
    }
    manifest_path = artifact_dir / "command-manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    failures = [record for record in records if record["exit_code"] != 0]
    return 1 if failures or len(records) != len(w04_commands(args)) else 0


if __name__ == "__main__":
    sys.exit(main())
