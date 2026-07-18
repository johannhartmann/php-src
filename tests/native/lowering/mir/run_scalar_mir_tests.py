#!/usr/bin/env python3
"""Build and run the strict standalone W03 scalar MIR suite."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[4]
SOURCES = (
    "Zend/Native/MIR/Core/zend_mir_arena.c",
    "Zend/Native/MIR/Core/zend_mir_ids.c",
    "Zend/Native/MIR/Core/zend_mir_module.c",
    "Zend/Native/MIR/Core/zend_mir_view.c",
    "Zend/Native/MIR/Scalar/zend_mir_scalar_descriptors.c",
    "Zend/Native/MIR/Scalar/zend_mir_value_facts.c",
    "Zend/Native/MIR/Scalar/zend_mir_verify_scalar.c",
    "Zend/Native/MIR/Text/zend_mir_dump.c",
    "Zend/Native/MIR/Verify/zend_mir_verify.c",
    "Zend/Native/MIR/Verify/zend_mir_verify_ids.c",
    "Zend/Native/MIR/Verify/zend_mir_verify_cfg.c",
    "Zend/Native/MIR/Verify/zend_mir_verify_dominance.c",
    "Zend/Native/MIR/Verify/zend_mir_verify_semantics.c",
    "Zend/Native/MIR/Verify/zend_mir_verify_frames.c",
    "Zend/Native/MIR/Semantics/zend_mir_semantic_catalog.c",
    "Zend/Native/MIR/Semantics/zend_mir_effect_summary.c",
    "Zend/Native/MIR/Semantics/zend_mir_alias.c",
    "Zend/Native/MIR/Semantics/zend_mir_ownership.c",
    "tests/native/mir/contracts/fixture_host.c",
    "tests/native/mir/verify/fixtures/verify_fixtures.c",
    "tests/native/mir/text/mir_test_parser.c",
    "tests/native/lowering/mir/test_scalar_mir.c",
)


def command(value: str) -> list[str]:
    parsed = shlex.split(value)
    if not parsed:
        raise ValueError("compiler command must not be empty")
    return parsed


def run(arguments: list[str], environment: dict[str, str]) -> None:
    print("+ " + " ".join(shlex.quote(part) for part in arguments), flush=True)
    subprocess.run(arguments, cwd=ROOT, env=environment, check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    try:
        cc = command(args.cc)
        cxx = command(os.environ.get("CXX", "c++"))
    except ValueError as error:
        parser.error(str(error))

    environment = os.environ.copy()
    environment.update({"LC_ALL": "C", "TZ": "UTC"})
    warnings = ["-Wall", "-Wextra", "-Wpedantic", "-Werror", "-I."]

    with tempfile.TemporaryDirectory(prefix="zend-mir-scalar-") as directory:
        temporary = Path(directory)
        executable = temporary / "test_scalar_mir"
        run(
            cc
            + ["-std=c11", "-DZEND_MIR_VERIFY_TESTING"]
            + warnings
            + list(SOURCES)
            + ["-o", str(executable)],
            environment,
        )
        run([str(executable)], environment)

        header_object = temporary / "test_scalar_headers.o"
        run(
            cxx
            + ["-std=c++20"]
            + warnings
            + [
                "-c",
                "tests/native/lowering/mir/test_scalar_headers.cpp",
                "-o",
                str(header_object),
            ],
            environment,
        )

    print("W03 scalar MIR C11, C++20, text, and verifier suite passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
