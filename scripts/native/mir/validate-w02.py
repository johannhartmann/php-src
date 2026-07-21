#!/usr/bin/env python3
"""Run the W02 MIR contract, implementation, and unit validation."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[3]
MIR = ROOT / "Zend/Native/MIR"
FORBIDDEN_INCLUDE = re.compile(
    r"^\s*#\s*include\s*[<\"][^>\"]*"
    r"(?:tpde|dynasm|x86|amd64|aarch64|arm64|riscv|"
    r"elf|macho|coff|zend_jit|zend_vm)[^>\"]*[>\"]",
    re.IGNORECASE | re.MULTILINE,
)
FORBIDDEN_TOKEN = re.compile(r"\b(?:TPDE|DynASM)\b", re.IGNORECASE)


class W02Error(RuntimeError):
    """A deterministic W02 validation failure."""


def production_files() -> tuple[Path, ...]:
    return tuple(
        sorted(
            path
            for pattern in ("*.c", "*.h")
            for path in MIR.rglob(pattern)
            if "ControlFlow" not in path.relative_to(MIR).parts
        )
    )


def architecture_leaks() -> list[str]:
    leaks: list[str] = []
    for path in production_files():
        text = path.read_text(encoding="utf-8")
        include = FORBIDDEN_INCLUDE.search(text)
        token = FORBIDDEN_TOKEN.search(text)
        if include is not None:
            leaks.append(
                f"{path.relative_to(ROOT)}: forbidden include "
                f"{include.group(0).strip()}"
            )
        elif token is not None:
            leaks.append(
                f"{path.relative_to(ROOT)}: forbidden backend token {token.group(0)}"
            )
    return leaks


def run(command: list[str], environment: dict[str, str]) -> None:
    print("+ " + " ".join(shlex.quote(part) for part in command), flush=True)
    subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        check=True,
        timeout=180,
    )


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
    for command in (
        ["python3", "scripts/native/mir/check-contract.py", "--check"],
        ["python3", "scripts/native/mir/generate-semantic-ids.py", "--check"],
        ["python3", "scripts/native/mir/test-w02.py", "--cc", cc],
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
    ):
        run(command, environment)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()
    if not arguments.check:
        parser.error("only --check is supported")
    try:
        validate()
        print("W02 validation passed")
    except (
        OSError,
        subprocess.CalledProcessError,
        subprocess.TimeoutExpired,
        W02Error,
    ) as error:
        print(f"W02 validation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
