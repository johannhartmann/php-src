#!/usr/bin/env python3
"""Validate the W03 scalar-lowering implementation and its live profile."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
LOWERING = ROOT / "Zend/Native/Lowering"
PROFILE_PATH = ROOT / "docs/native-engine/lowering/w03-opcode-profile.json"

PROVIDER_PATHS = {
    "core": (
        "Zend/Native/Lowering/Core/zend_mir_lowering_registry.c",
        "Zend/Native/Lowering/Core/zend_mir_lowering_providers.c",
    ),
    "frontend": (
        "Zend/Native/Lowering/Frontend/zend_mir_zend_source.c",
        "Zend/Native/Lowering/Frontend/zend_mir_value_facts.c",
    ),
    "numeric": (
        "Zend/Native/Lowering/Scalar/Numeric/zend_mir_numeric_provider.c",
    ),
    "logic": (
        "Zend/Native/Lowering/Scalar/Logic/zend_mir_logic_provider.c",
    ),
    "lifetime": (
        "Zend/Native/Lowering/StraightLine/zend_mir_lifetime_provider.c",
    ),
}
REQUIRED_PATHS = (
    "Zend/Native/MIR/Scalar/zend_mir_verify_scalar.c",
    "Zend/Native/Lowering/Core/zend_mir_lowering_providers.c",
    "ext/native_mir_test/config.m4",
    "ext/native_mir_test/native_mir_test.c",
    "scripts/native/lowering/dump-mir.py",
    "scripts/native/lowering/run-w03-differential.py",
)
FORBIDDEN_INCLUDE = re.compile(
    r"^\s*#\s*include\s*[<\"][^>\"]*"
    r"(?:tpde|dynasm|x86|amd64|aarch64|arm64|riscv|"
    r"elf|macho|coff|zend_jit|zend_vm_execute)[^>\"]*[>\"]",
    re.IGNORECASE | re.MULTILINE,
)
FORBIDDEN_RUNTIME = re.compile(
    r"\b(?:TPDE|DynASM|zend_execute|execute_ex|zend_vm_call_opcode_handler|"
    r"mir_interpret|mir_evaluate)\b",
    re.IGNORECASE,
)


class W03Error(RuntimeError):
    """A deterministic W03 validation failure."""


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise W03Error(f"{path.relative_to(ROOT)}: {error}") from error


def production_paths() -> tuple[Path, ...]:
    paths = [
        *(
            path
            for path in LOWERING.rglob("*.c")
            if "ControlFlow" not in path.relative_to(LOWERING).parts
        ),
        *(
            path
            for path in LOWERING.rglob("*.h")
            if "ControlFlow" not in path.relative_to(LOWERING).parts
            and path.name != "zend_mir_lowering_w04.h"
        ),
        ROOT / "ext/native_mir_test/config.m4",
        ROOT / "ext/native_mir_test/native_mir_test.c",
        ROOT / "ext/native_mir_test/native_mir_test.stub.php",
    ]
    return tuple(sorted(paths))


def architecture_leaks() -> list[str]:
    leaks: list[str] = []
    for path in production_paths():
        text = path.read_text(encoding="utf-8")
        include = FORBIDDEN_INCLUDE.search(text)
        runtime = FORBIDDEN_RUNTIME.search(text)
        if include is not None:
            leaks.append(
                f"{path.relative_to(ROOT)}: forbidden include "
                f"{include.group(0).strip()}"
            )
        elif runtime is not None:
            leaks.append(
                f"{path.relative_to(ROOT)}: forbidden runtime token "
                f"{runtime.group(0)}"
            )
    return leaks


def validate_profile(profile: dict[str, Any]) -> None:
    entries = profile.get("opcodes")
    if not isinstance(entries, list) or len(entries) != profile.get(
        "active_opcode_count"
    ):
        raise W03Error("profile opcode count does not match active_opcode_count")
    numbers = [entry.get("number") for entry in entries]
    if numbers != sorted(numbers) or len(numbers) != len(set(numbers)):
        raise W03Error("profile opcode numbers are not sorted and unique")
    proof_catalog = set(profile.get("proof_catalog", []))
    accepted = [entry for entry in entries if entry.get("classification") != "deferred"]
    if not accepted:
        raise W03Error("profile has no accepted opcodes")
    for entry in entries:
        classification = entry.get("classification")
        if classification not in {"required", "conditional", "deferred"}:
            raise W03Error(f"invalid classification for {entry.get('opcode')}")
        unknown = set(entry.get("proofs", [])) - proof_catalog
        if unknown:
            raise W03Error(
                f"{entry.get('opcode')} references unknown proofs: {sorted(unknown)}"
            )
        provider = entry.get("provider")
        if classification != "deferred":
            if provider not in PROVIDER_PATHS:
                raise W03Error(f"accepted opcode has no implementation provider: {provider}")
            if entry.get("deferred_wave") is not None:
                raise W03Error(f"accepted opcode is also deferred: {entry.get('opcode')}")
        elif provider is not None:
            raise W03Error(f"deferred opcode claims provider: {entry.get('opcode')}")
        elif entry.get("mir_opcodes"):
            raise W03Error(f"deferred opcode publishes MIR: {entry.get('opcode')}")
    for provider in sorted({entry["provider"] for entry in accepted}):
        for value in PROVIDER_PATHS[provider]:
            if not (ROOT / value).is_file():
                raise W03Error(f"missing provider implementation: {value}")
    for value in REQUIRED_PATHS:
        if not (ROOT / value).is_file():
            raise W03Error(f"missing required implementation: {value}")


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
        raise W03Error("\n".join(leaks))
    validate_profile(load_json(PROFILE_PATH))
    environment = os.environ.copy()
    environment.update(
        {
            "LANG": "C",
            "LC_ALL": "C",
            "TZ": "UTC",
            "PYTHONDONTWRITEBYTECODE": "1",
        }
    )
    for command in (
        ["python3", "scripts/native/lowering/generate-profile.py", "--check"],
        ["python3", "tests/native/lowering/corpus/validate_manifest.py", "--check"],
        ["python3", "scripts/native/lowering/check-contract.py", "--check"],
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
        print("W03 lowering validation passed")
    except (
        OSError,
        subprocess.CalledProcessError,
        subprocess.TimeoutExpired,
        W03Error,
    ) as error:
        print(f"W03 lowering validation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
