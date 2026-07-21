#!/usr/bin/env python3
"""Build and run the storage-independent W03 core lowering tests."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[4]
CORE = ROOT / "Zend" / "Native" / "Lowering" / "Core"
TEST = ROOT / "tests" / "native" / "lowering" / "core" / "test_core_lowering.c"
SOURCES = [
    CORE / "zend_mir_lowering.c",
    CORE / "zend_mir_lowering_registry.c",
    CORE / "zend_mir_lowering_context.c",
    CORE / "zend_mir_lowering_diagnostics.c",
    TEST,
]


def run(command: list[str], *, input_text: str | None = None) -> None:
    print("+", shlex.join(command), flush=True)
    subprocess.run(
        command,
        cwd=ROOT,
        check=True,
        input=input_text,
        text=True,
        env={**os.environ, "LC_ALL": "C"},
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    cc = shlex.split(args.cc)
    if not cc:
        parser.error("--cc must name a compiler")
    cxx = shlex.split(os.environ.get("CXX", "c++"))

    common = [
        "-Wall",
        "-Wextra",
        "-Werror",
        "-pedantic",
        "-I",
        str(ROOT / "Zend" / "Native" / "Lowering"),
        "-I",
        str(ROOT / "Zend" / "Native" / "MIR"),
    ]
    with tempfile.TemporaryDirectory(prefix="zend-mir-lowering-core-") as temp:
        executable = Path(temp) / "test_core_lowering"
        run([*cc, "-std=c11", *common, *(str(path) for path in SOURCES), "-o", str(executable)])
        run([str(executable)])

        object_file = Path(temp) / "header_smoke.o"
        run(
            [
                *cxx,
                "-std=c++20",
                *common,
                "-x",
                "c++",
                "-c",
                "-o",
                str(object_file),
                "-",
            ],
            input_text=(
                '#include "Core/zend_mir_lowering_internal.h"\n'
                "int main() { return 0; }\n"
            ),
        )

    print("core lowering harness: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
