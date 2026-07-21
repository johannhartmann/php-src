#!/usr/bin/env python3
"""Build and run the standalone scalar logic tests."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[4]
LOGIC_SOURCES = (
    "Zend/Native/Lowering/Scalar/Logic/zend_mir_logic_proofs.c",
    "Zend/Native/Lowering/Scalar/Logic/zend_mir_lower_compare.c",
    "Zend/Native/Lowering/Scalar/Logic/zend_mir_lower_boolean.c",
    "Zend/Native/Lowering/Scalar/Logic/zend_mir_lower_cast.c",
    "Zend/Native/Lowering/Scalar/Logic/zend_mir_logic_provider.c",
)
TEST_SOURCE = "tests/native/lowering/logic/test_logic_lowering.c"


def compiler_command(value: str) -> list[str]:
    command = shlex.split(value)
    if not command:
        raise ValueError("compiler command must not be empty")
    executable = shutil.which(command[0])
    if executable is None:
        raise ValueError(f"compiler not found: {command[0]}")
    command[0] = executable
    return command


def run(command: list[str], environment: dict[str, str]) -> None:
    print("+ " + " ".join(shlex.quote(part) for part in command), flush=True)
    subprocess.run(command, cwd=ROOT, env=environment, check=True, timeout=60)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
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

    with tempfile.TemporaryDirectory(prefix="zend-mir-logic-") as directory:
        temporary = Path(directory)
        executable = temporary / "test_logic_lowering"
        run(
            cc
            + ["-std=c11"]
            + common
            + list(LOGIC_SOURCES)
            + [TEST_SOURCE, "-o", str(executable)],
            environment,
        )
        run([str(executable)], environment)

        header_source = temporary / "logic_header.cpp"
        header_source.write_text(
            '#include "Zend/Native/Lowering/Scalar/Logic/zend_mir_logic.h"\n'
            "int main() { return 0; }\n",
            encoding="ascii",
        )
        run(
            cxx
            + ["-std=c++20"]
            + common
            + ["-c", str(header_source), "-o", str(temporary / "logic_header.o")],
            environment,
        )

    print("scalar logic tests passed: C11 lowering and C++20 header")
    return 0


if __name__ == "__main__":
    sys.exit(main())
