#!/usr/bin/env python3
"""Build every ZNMIR production source and run the W02 integration binary."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[3]
PRODUCTION = tuple(
    str(path.relative_to(ROOT))
    for path in sorted((ROOT / "Zend/Native/MIR").rglob("*.c"))
)
TEST_SOURCES = (
    "tests/native/mir/contracts/fixture_host.c",
    "tests/native/mir/text/text_fixtures.c",
    "tests/native/mir/verify/fixtures/verify_fixtures.c",
    "tests/native/mir/integration/test_w02_integration.c",
)


def compiler_command(value: str) -> list[str]:
    command = shlex.split(value)
    if not command:
        raise ValueError("compiler command must not be empty")
    return command


def warning_flags(compiler: list[str]) -> list[str]:
    flags = ["-Wall", "-Wextra", "-Wpedantic", "-Werror"]
    version = subprocess.run(
        compiler + ["--version"],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
        timeout=10,
    )
    if "Apple clang" in version.stdout + version.stderr:
        # The frozen W01 enums intentionally use UINT32_MAX sentinels in C11.
        flags.append("-Wno-c23-extensions")
    return flags


def run(command: list[str], environment: dict[str, str]) -> None:
    print("+ " + " ".join(shlex.quote(part) for part in command), flush=True)
    subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        check=True,
        timeout=60,
    )


def sanitizer_flags(name: str | None) -> list[str]:
    if name is None:
        return []
    return [f"-fsanitize={name}", "-fno-omit-frame-pointer", "-O1"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument(
        "--sanitizer", choices=("address", "undefined"), default=None
    )
    arguments = parser.parse_args()
    try:
        cc = compiler_command(arguments.cc)
        cxx = compiler_command(os.environ.get("CXX", "c++"))
    except ValueError as error:
        parser.error(str(error))

    environment = os.environ.copy()
    environment.update(
        {
            "LC_ALL": "C",
            "TZ": "UTC",
            "ASAN_OPTIONS": "abort_on_error=1",
            "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
        }
    )
    common = [*warning_flags(cc), "-I."]
    sanitizer = sanitizer_flags(arguments.sanitizer)

    with tempfile.TemporaryDirectory(prefix="zend-mir-w02-") as directory:
        temporary = Path(directory)
        executable = temporary / "test_w02_integration"
        run(
            cc
            + ["-std=c11", "-DZEND_MIR_VERIFY_TESTING"]
            + common
            + sanitizer
            + list(PRODUCTION)
            + list(TEST_SOURCES)
            + ["-o", str(executable)],
            environment,
        )
        run([str(executable)], environment)

        c_header_object = temporary / "test_w02_headers_c.o"
        run(
            cc
            + ["-std=c11"]
            + common
            + sanitizer
            + [
                "-c",
                "tests/native/mir/integration/test_w02_headers.c",
                "-o",
                str(c_header_object),
            ],
            environment,
        )

        header_object = temporary / "test_w02_headers.o"
        run(
            cxx
            + ["-std=c++20"]
            + common
            + sanitizer
            + [
                "-c",
                "tests/native/mir/integration/test_w02_headers.cpp",
                "-o",
                str(header_object),
            ],
            environment,
        )

    mode = arguments.sanitizer or "strict"
    print(
        f"W02 standalone integration passed ({len(PRODUCTION)} production "
        f"sources, mode={mode})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
