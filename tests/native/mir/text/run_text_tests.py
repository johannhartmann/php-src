#!/usr/bin/env python3
"""Compile and run the dependency-free znmir-text-v1 contract tests."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[4]
GOLDEN = ROOT / "tests/native/mir/golden"
FIXTURES = (
    "linear",
    "diamond",
    "loop",
    "ownership",
    "exception-statepoint",
    "nested-frame",
)
SOURCES = (
    "Zend/Native/MIR/Text/zend_mir_dump.c",
    "tests/native/mir/contracts/fixture_host.c",
    "tests/native/mir/text/mir_test_parser.c",
    "tests/native/mir/text/text_fixtures.c",
    "tests/native/mir/text/text_test_main.c",
)


def run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(command, cwd=ROOT, check=True, timeout=60, **kwargs)


def c_warning_flags(compiler: str) -> list[str]:
    flags = ["-Wall", "-Wextra", "-Wpedantic", "-Werror"]
    version = subprocess.run(
        [compiler, "--version"],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
        timeout=10,
    )
    if "Apple clang" in version.stdout + version.stderr:
        # The pinned W01 headers use UINT32_MAX enum sentinels. Apple Clang
        # diagnoses those as a C23 extension, while the W01 contract compiler
        # intentionally accepts them in C11 mode.
        flags.append("-Wno-c23-extensions")
    return flags


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="znmir-text-") as temporary:
        temp = Path(temporary)
        executable = temp / "text-tests"
        command = [
            args.cc,
            "-std=c11",
            *c_warning_flags(args.cc),
            "-I.",
            *SOURCES,
            "-o",
            str(executable),
        ]
        run(command)
        self_test = run([str(executable), "--self-test"], stdout=subprocess.PIPE)
        print(self_test.stdout.decode("ascii").strip())

        expected_hashes: dict[str, str] = {}
        for line in (GOLDEN / "SHA256SUMS").read_text(encoding="ascii").splitlines():
            digest, filename = line.split("  ", 1)
            expected_hashes[filename] = digest

        for fixture in FIXTURES:
            emitted = run(
                [str(executable), "--emit", fixture], stdout=subprocess.PIPE
            ).stdout
            filename = f"{fixture}.znmir"
            expected = (GOLDEN / filename).read_bytes()
            if emitted != expected:
                raise SystemExit(f"golden mismatch: {filename}")
            digest = hashlib.sha256(emitted).hexdigest()
            if expected_hashes.get(filename) != digest:
                raise SystemExit(f"SHA-256 mismatch: {filename}")
            print(f"{digest}  {filename}")

        cpp = temp / "header.cpp"
        cpp.write_text(
            '#include "Zend/Native/MIR/Text/zend_mir_dump.h"\nint main() { return 0; }\n',
            encoding="ascii",
        )
        cxx = os.environ.get("CXX", "c++")
        run(
            [
                cxx,
                "-std=c++20",
                "-Wall",
                "-Wextra",
                "-Wpedantic",
                "-Werror",
                "-I.",
                str(cpp),
                "-c",
                "-o",
                str(temp / "header.o"),
            ]
        )

    print("znmir-text-v1 golden, digest, roundtrip, and header tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
