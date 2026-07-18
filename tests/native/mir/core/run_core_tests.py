#!/usr/bin/env python3
"""Compile and run the standalone W02-A MIR core tests."""

from __future__ import annotations

import argparse
import shlex
import subprocess
import tempfile
from pathlib import Path


CORE_SOURCES = (
    "zend_mir_arena.c",
    "zend_mir_ids.c",
    "zend_mir_module.c",
    "zend_mir_view.c",
)
SCALAR_SOURCES = (
    "zend_mir_scalar_descriptors.c",
    "zend_mir_value_facts.c",
)


def run(command: list[str], cwd: Path) -> None:
    subprocess.run(command, cwd=cwd, check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="cc", help="C compiler command")
    args = parser.parse_args()

    repo = Path(__file__).resolve().parents[4]
    core = repo / "Zend" / "Native" / "MIR" / "Core"
    scalar = repo / "Zend" / "Native" / "MIR" / "Scalar"
    mir = repo / "Zend" / "Native" / "MIR"
    test_source = Path(__file__).with_name("test_core.c")
    compiler = shlex.split(args.cc)
    if not compiler:
        parser.error("--cc must name a compiler")

    with tempfile.TemporaryDirectory(prefix="znmir-core-") as directory:
        temporary = Path(directory)
        executable = temporary / "test_core"
        command = [
            *compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            "-Werror",
            "-I",
            str(repo),
            "-I",
            str(mir),
            "-I",
            str(core),
            *(str(core / source) for source in CORE_SOURCES),
            *(str(scalar / source) for source in SCALAR_SOURCES),
            str(test_source),
            "-o",
            str(executable),
        ]
        run(command, repo)
        run([str(executable)], repo)

        run(
            [
                *compiler,
                "-x",
                "c++",
                "-std=c++20",
                "-Wall",
                "-Wextra",
                "-Wpedantic",
                "-Werror",
                "-I",
                str(mir),
                "-I",
                str(core),
                "-include",
                "zend_mir_arena.h",
                "-c",
                "/dev/null",
                "-o",
                str(temporary / "test_core_header.o"),
            ],
            repo,
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
