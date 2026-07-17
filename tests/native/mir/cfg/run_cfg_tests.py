#!/usr/bin/env python3
"""Compile and run the target-neutral ZNMIR CFG test suite."""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


REPOSITORY_ROOT = Path(__file__).resolve().parents[4]
CFG_SOURCES = (
    "Zend/Native/MIR/CFG/zend_mir_cfg.c",
    "Zend/Native/MIR/CFG/zend_mir_phi.c",
    "Zend/Native/MIR/CFG/zend_mir_dominance.c",
)
TEST_SOURCES = (
    "tests/native/mir/contracts/fixture_host.c",
    "tests/native/mir/cfg/test_cfg.c",
)
PUBLIC_HEADERS = (
    "Zend/Native/MIR/CFG/zend_mir_cfg.h",
    "Zend/Native/MIR/CFG/zend_mir_phi.h",
    "Zend/Native/MIR/CFG/zend_mir_dominance.h",
)


def run(command: list[str], *, environment: dict[str, str]) -> None:
    printable = " ".join(shlex.quote(part) for part in command)
    print(f"+ {printable}", flush=True)
    subprocess.run(command, cwd=REPOSITORY_ROOT, env=environment, check=True)


def compiler_command(value: str) -> list[str]:
    command = shlex.split(value)
    if not command:
        raise ValueError("compiler command must not be empty")
    return command


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    arguments = parser.parse_args()

    try:
        cc = compiler_command(arguments.cc)
    except ValueError as error:
        parser.error(str(error))

    environment = os.environ.copy()
    environment.update({"LC_ALL": "C", "TZ": "UTC"})

    with tempfile.TemporaryDirectory(prefix="zend-mir-cfg-") as directory:
        temporary = Path(directory)
        executable = temporary / "test_cfg"
        common = ["-Wall", "-Wextra", "-Werror", "-I."]

        run(
            cc
            + ["-std=c11"]
            + common
            + list(CFG_SOURCES)
            + list(TEST_SOURCES)
            + ["-o", str(executable)],
            environment=environment,
        )
        run([str(executable)], environment=environment)

        for language, standard, extension in (
            ("c", "c11", "c"),
            ("c++", "c++20", "cpp"),
        ):
            source = temporary / f"public_headers.{extension}"
            source.write_text(
                "".join(f'#include "{header}"\n' for header in PUBLIC_HEADERS)
                + "int main(void) { return 0; }\n",
                encoding="utf-8",
            )
            output = temporary / f"public_headers_{language}.o"
            run(
                cc
                + [f"-std={standard}", "-x", language]
                + common
                + ["-c", str(source), "-o", str(output)],
                environment=environment,
            )

    print("CFG tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
