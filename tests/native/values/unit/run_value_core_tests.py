#!/usr/bin/env python3
"""Compile and run the standalone W06-A1 value/reference core tests."""

from __future__ import annotations

import argparse
import os
import shlex
import subprocess
import tempfile
from pathlib import Path


def run(command: list[str], cwd: Path) -> None:
    subprocess.run(command, cwd=cwd, check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    compiler = shlex.split(args.cc)
    if not compiler:
        parser.error("--cc must name a compiler")

    repo = Path(__file__).resolve().parents[4]
    mir = repo / "Zend" / "Native" / "MIR"
    core = mir / "Core"
    scalar = mir / "Scalar"
    value_core = repo / "Zend" / "Native" / "Values" / "Core"
    sources = [
        core / "zend_mir_arena.c",
        core / "zend_mir_ids.c",
        core / "zend_mir_module.c",
        core / "zend_mir_view.c",
        scalar / "zend_mir_scalar_descriptors.c",
        scalar / "zend_mir_value_facts.c",
        value_core / "zend_mir_value_core.c",
        mir / "Values" / "zend_mir_verify_values.c",
        mir / "Text" / "zend_mir_dump.c",
        Path(__file__).with_name("test_value_core.c"),
    ]

    with tempfile.TemporaryDirectory(prefix="znmir-w06-core-") as directory:
        temporary = Path(directory)
        executable = temporary / "test_value_core"
        run(
            [
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
                *(str(source) for source in sources),
                "-o",
                str(executable),
            ],
            repo,
        )
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
                str(repo),
                "-I",
                str(mir),
                "-include",
                str(value_core / "zend_mir_value_core.h"),
                "-include",
                str(mir / "zend_mir_values.h"),
                "-c",
                "/dev/null",
                "-o",
                str(temporary / "value_headers.o"),
            ],
            repo,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
