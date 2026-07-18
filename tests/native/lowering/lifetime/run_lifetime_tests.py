#!/usr/bin/env python3
"""Build and run the standalone W03-E lifetime lowering tests."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[4]
LOWERING_SOURCES = (
    "Zend/Native/Lowering/StraightLine/zend_mir_lower_structural.c",
    "Zend/Native/Lowering/StraightLine/zend_mir_lower_copy_move.c",
    "Zend/Native/Lowering/StraightLine/zend_mir_lower_entry_state.c",
    "Zend/Native/Lowering/StraightLine/zend_mir_lower_return.c",
    "Zend/Native/Lowering/StraightLine/zend_mir_lifetime_provider.c",
)
VERIFY_SOURCES = (
    "Zend/Native/MIR/Verify/zend_mir_verify.c",
    "Zend/Native/MIR/Verify/zend_mir_verify_ids.c",
    "Zend/Native/MIR/Verify/zend_mir_verify_cfg.c",
    "Zend/Native/MIR/Verify/zend_mir_verify_dominance.c",
    "Zend/Native/MIR/Verify/zend_mir_verify_semantics.c",
    "Zend/Native/MIR/Verify/zend_mir_verify_frames.c",
)
SEMANTIC_SOURCES = (
    "Zend/Native/MIR/Semantics/zend_mir_semantic_catalog.c",
    "Zend/Native/MIR/Semantics/zend_mir_effect_summary.c",
    "Zend/Native/MIR/Semantics/zend_mir_alias.c",
    "Zend/Native/MIR/Semantics/zend_mir_ownership.c",
)
SCALAR_SOURCES = (
    "Zend/Native/MIR/Scalar/zend_mir_scalar_descriptors.c",
)


def compiler(value: str) -> list[str]:
    command = shlex.split(value)
    if not command:
        raise ValueError("compiler command must not be empty")
    return command


def run(command: list[str], environment: dict[str, str]) -> None:
    print("+ " + " ".join(shlex.quote(part) for part in command), flush=True)
    subprocess.run(command, cwd=ROOT, env=environment, check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    arguments = parser.parse_args()
    try:
        cc = compiler(arguments.cc)
        cxx = compiler(os.environ.get("CXX", "c++"))
    except ValueError as error:
        parser.error(str(error))

    environment = os.environ.copy()
    environment.update({"LC_ALL": "C", "TZ": "UTC"})
    common = ["-Wall", "-Wextra", "-Wpedantic", "-Werror", "-I."]

    with tempfile.TemporaryDirectory(prefix="zend-mir-lifetime-") as directory:
        temporary = Path(directory)
        executable = temporary / "test_lifetime"
        run(
            cc
            + ["-std=c11", "-DZEND_MIR_VERIFY_TESTING"]
            + common
            + list(LOWERING_SOURCES)
            + list(VERIFY_SOURCES)
            + list(SEMANTIC_SOURCES)
            + list(SCALAR_SOURCES)
            + [
                "tests/native/mir/contracts/fixture_host.c",
                "tests/native/lowering/lifetime/test_lifetime.c",
                "-o",
                str(executable),
            ],
            environment,
        )
        run([str(executable)], environment)
        run(
            cxx
            + ["-std=c++20"]
            + common
            + [
                "-c",
                "tests/native/lowering/lifetime/test_lifetime_header.cpp",
                "-o",
                str(temporary / "test_lifetime_header.o"),
            ],
            environment,
        )
    print("W03-E lifetime lowering tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
