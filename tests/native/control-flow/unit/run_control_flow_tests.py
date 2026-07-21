#!/usr/bin/env python3
from __future__ import annotations

import argparse
import pathlib
import shlex
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[4]
PRODUCTION = [
    "Zend/Native/MIR/ControlFlow/zend_mir_control_flow_map.c",
    "Zend/Native/MIR/ControlFlow/zend_mir_verify_control_flow.c",
    "Zend/Native/Lowering/ControlFlow/zend_mir_control_flow_proofs.c",
    "Zend/Native/Lowering/ControlFlow/zend_mir_control_flow_provider.c",
    "Zend/Native/Lowering/ControlFlow/zend_mir_lower_control_flow.c",
]
FRONTEND = [
    "Zend/Native/Lowering/Frontend/zend_mir_zend_source.c",
    "Zend/Native/Lowering/Frontend/zend_mir_operand_map.c",
    "Zend/Native/Lowering/Frontend/zend_mir_value_facts.c",
    "Zend/Native/Lowering/Frontend/zend_mir_literal_pool.c",
    "Zend/Native/Lowering/Frontend/zend_mir_slot_map.c",
    "Zend/Native/Lowering/Frontend/zend_mir_source_positions.c",
    "tests/native/lowering/frontend/zend_optimizer_stubs.c",
]
WARNINGS = [
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wno-c23-extensions",
]
FRONTEND_FLAGS = [
    "-D_GNU_SOURCE",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-Wno-sign-compare",
]


def run(command: list[str]) -> None:
    print("+", shlex.join(command), flush=True)
    subprocess.run(command, cwd=ROOT, check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    cc = shlex.split(args.cc)
    cxx = shlex.split(args.cxx)
    with tempfile.TemporaryDirectory(prefix="w04-control-flow-") as directory:
        temp = pathlib.Path(directory)
        for source in PRODUCTION:
            run(
                [
                    *cc,
                    "-std=c11",
                    *WARNINGS,
                    "-I.",
                    "-c",
                    source,
                    "-o",
                    str(temp / (pathlib.Path(source).stem + ".o")),
                ]
            )
        binary = temp / "test_control_flow"
        run(
            [
                *cc,
                "-std=c11",
                *WARNINGS,
                "-I.",
                "Zend/Native/MIR/ControlFlow/zend_mir_control_flow_map.c",
                "Zend/Native/MIR/ControlFlow/zend_mir_verify_control_flow.c",
                "Zend/Native/MIR/Scalar/zend_mir_scalar_descriptors.c",
                "Zend/Native/Lowering/Core/zend_mir_lowering_context.c",
                "Zend/Native/Lowering/Core/zend_mir_lowering_registry.c",
                "Zend/Native/Lowering/ControlFlow/zend_mir_control_flow_proofs.c",
                "Zend/Native/Lowering/ControlFlow/zend_mir_control_flow_provider.c",
                "Zend/Native/Lowering/ControlFlow/zend_mir_lower_control_flow.c",
                "tests/native/mir/contracts/fixture_host.c",
                "tests/native/control-flow/unit/test_control_flow.c",
                "-o",
                str(binary),
            ]
        )
        run([str(binary)])
        frontend_binary = temp / "test_frontend_w04"
        run(
            [
                *cc,
                "-std=c11",
                *FRONTEND_FLAGS,
                "-Itests/native/lowering/frontend/include",
                "-I.",
                "-IZend",
                *FRONTEND,
                "tests/native/control-flow/unit/test_frontend_w04.c",
                "-o",
                str(frontend_binary),
            ]
        )
        run([str(frontend_binary)])
        run(
            [
                *cxx,
                "-std=c++20",
                *WARNINGS,
                "-I.",
                "tests/native/control-flow/unit/test_control_flow_headers.cpp",
                "-o",
                str(temp / "test_control_flow_headers"),
            ]
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
