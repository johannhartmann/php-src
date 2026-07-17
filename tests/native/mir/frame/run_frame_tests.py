#!/usr/bin/env python3
"""Compile and run the standalone W02-D frame-state tests."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[4]
FRAME = ROOT / "Zend" / "Native" / "MIR" / "Frame"
TESTS = ROOT / "tests" / "native" / "mir" / "frame"


def run(command: list[str]) -> None:
    completed = subprocess.run(
        command, cwd=ROOT, text=True, capture_output=True, check=False
    )
    if completed.returncode != 0:
        output = "\n".join(part for part in (completed.stdout, completed.stderr) if part)
        raise RuntimeError(f"command failed ({' '.join(command)}):\n{output.rstrip()}")


def compiler(requested: str) -> str:
    resolved = shutil.which(requested)
    if resolved is None:
        raise RuntimeError(f"required compiler not found: {requested}")
    return resolved


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    arguments = parser.parse_args()
    cc = compiler(arguments.cc)
    cxx = compiler(os.environ.get("CXX", "c++"))
    sources = [
        FRAME / "zend_mir_frame_state.c",
        FRAME / "zend_mir_frame_intern.c",
        FRAME / "zend_mir_source_map.c",
        TESTS / "test_frame_state.c",
    ]
    with tempfile.TemporaryDirectory(prefix="znmir-frame-") as temporary:
        temp = Path(temporary)
        executable = temp / "frame-tests"
        common = ["-Wall", "-Wextra", "-Werror", "-I", str(ROOT)]
        run([cc, "-std=c11", *common, *(str(path) for path in sources), "-o", str(executable)])
        run([str(executable)])

        header_smoke = temp / "header-smoke.cc"
        header_smoke.write_text(
            '#include "Zend/Native/MIR/Frame/zend_mir_frame_state.h"\n'
            '#include "Zend/Native/MIR/Frame/zend_mir_source_map.h"\n'
            "int main() { return 0; }\n",
            encoding="utf-8",
        )
        run([cxx, "-std=c++20", *common, str(header_smoke), "-o", str(temp / "header-smoke")])
    print("ZNMIR frame tests passed: C11 runtime and C++20 headers are valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
