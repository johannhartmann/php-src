#!/usr/bin/env python3
"""Run deterministic in-process mutations against the test-only parser."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[4]
SOURCES = (
    "Zend/Native/MIR/Text/zend_mir_dump.c",
    "tests/native/mir/contracts/fixture_host.c",
    "tests/native/mir/text/mir_test_parser.c",
    "tests/native/mir/text/text_fixtures.c",
    "tests/native/mir/fuzz/mir_text_mutation.c",
)


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
        # Match the W01 contract compiler for the pinned UINT32_MAX enum
        # sentinels without weakening diagnostics for the W02-E sources.
        flags.append("-Wno-c23-extensions")
    return flags


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--cases", type=int, required=True)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    if args.seed < 0 or args.cases < 1 or args.cases > 1_000_000:
        parser.error("seed must be nonnegative and cases must be in 1..1000000")

    with tempfile.TemporaryDirectory(prefix="znmir-fuzz-") as temporary:
        executable = Path(temporary) / "mutation-fuzz"
        subprocess.run(
            [
                args.cc,
                "-std=c11",
                "-O2",
                *c_warning_flags(args.cc),
                "-I.",
                *SOURCES,
                "-o",
                str(executable),
            ],
            cwd=ROOT,
            check=True,
            timeout=60,
        )
        subprocess.run(
            [str(executable), str(args.seed), str(args.cases)],
            cwd=ROOT,
            check=True,
            timeout=60,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
