#!/usr/bin/env python3
"""Build and run the standalone numeric lowering tests."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[4]
NUMERIC_SOURCES = (
    "Zend/Native/Lowering/Scalar/Numeric/zend_mir_lower_numeric.c",
    "Zend/Native/Lowering/Scalar/Numeric/zend_mir_numeric_proofs.c",
    "Zend/Native/Lowering/Scalar/Numeric/zend_mir_numeric_provider.c",
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

    with tempfile.TemporaryDirectory(prefix="zend-mir-numeric-") as directory:
        temporary = Path(directory)
        executable = temporary / "test_numeric"
        run(
            cc
            + ["-std=c11"]
            + common
            + list(NUMERIC_SOURCES)
            + [
                "tests/native/lowering/numeric/test_numeric.c",
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
                "tests/native/lowering/numeric/test_numeric_header.cpp",
                "-o",
                str(temporary / "test_numeric_header.o"),
            ],
            environment,
        )
    print("numeric lowering tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
