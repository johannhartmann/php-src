#!/usr/bin/env python3
"""Strict standalone build and runtime driver for the W02-F verifier."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


REPOSITORY_ROOT = Path(__file__).resolve().parents[4]
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
TEST_SOURCES = (
    "tests/native/mir/contracts/fixture_host.c",
    "tests/native/mir/verify/fixtures/verify_fixtures.c",
    "tests/native/mir/verify/test_verify.c",
)


def compiler_command(value: str) -> list[str]:
    command = shlex.split(value)
    if not command:
        raise ValueError("compiler command must not be empty")
    return command


def run(command: list[str], environment: dict[str, str]) -> None:
    print("+ " + " ".join(shlex.quote(part) for part in command), flush=True)
    subprocess.run(
        command,
        cwd=REPOSITORY_ROOT,
        env=environment,
        check=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    arguments = parser.parse_args()
    try:
        cc = compiler_command(arguments.cc)
        cxx = compiler_command(os.environ.get("CXX", "c++"))
    except ValueError as error:
        parser.error(str(error))

    environment = os.environ.copy()
    environment.update({"LC_ALL": "C", "TZ": "UTC"})
    common = ["-Wall", "-Wextra", "-Wpedantic", "-Werror", "-I."]

    with tempfile.TemporaryDirectory(prefix="zend-mir-verify-") as directory:
        temporary = Path(directory)
        executable = temporary / "test_verify"
        run(
            cc
            + ["-std=c11", "-DZEND_MIR_VERIFY_TESTING"]
            + common
            + list(VERIFY_SOURCES)
            + list(SEMANTIC_SOURCES)
            + list(TEST_SOURCES)
            + ["-o", str(executable)],
            environment,
        )
        run([str(executable)], environment)

        header_object = temporary / "test_verify_header.o"
        run(
            cxx
            + ["-std=c++20"]
            + common
            + ["-c", "tests/native/mir/verify/test_verify_header.cpp",
               "-o", str(header_object)],
            environment,
        )

    print("W02-F verifier tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
