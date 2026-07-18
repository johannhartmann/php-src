#!/usr/bin/env python3
"""Compile and run the W03 frontend source-view tests."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[4]
FRONTEND = ROOT / "Zend" / "Native" / "Lowering" / "Frontend"
TEST_DIR = Path(__file__).resolve().parent
SOURCES = (
    FRONTEND / "zend_mir_zend_source.c",
    FRONTEND / "zend_mir_operand_map.c",
    FRONTEND / "zend_mir_value_facts.c",
    FRONTEND / "zend_mir_literal_pool.c",
    FRONTEND / "zend_mir_slot_map.c",
    FRONTEND / "zend_mir_source_positions.c",
)


def run(command: list[str]) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=ROOT, check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="w03-frontend-") as temporary:
        temporary_dir = Path(temporary)
        executable = temporary_dir / "test_frontend"
        common_includes = [
            f"-I{TEST_DIR / 'include'}",
            f"-I{ROOT}",
            f"-I{ROOT / 'Zend'}",
        ]
        run(
            [
                args.cc,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                *common_includes,
                *(str(source) for source in SOURCES),
                str(TEST_DIR / "test_frontend.c"),
                "-o",
                str(executable),
            ]
        )
        run([str(executable)])

        cxx_source = temporary_dir / "test_frontend_header.cpp"
        cxx_source.write_text(
            """
#include "Zend/Native/Lowering/Frontend/zend_mir_zend_source.h"
#include <type_traits>
static_assert(std::is_standard_layout_v<zend_mir_zend_source>);
static_assert(std::is_trivially_copyable_v<zend_mir_source_position_ref>);
int main() { return 0; }
""".lstrip(),
            encoding="utf-8",
        )
        run(
            [
                args.cxx,
                "-std=c++20",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT}",
                str(cxx_source),
                "-o",
                str(temporary_dir / "test_frontend_header"),
            ]
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
