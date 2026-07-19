"""Unit tests for the W04 differential harness."""

from __future__ import annotations

import importlib.util
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[4]
RUNNER_PATH = ROOT / "scripts/native/control-flow/run-w04-differential.py"


def load_runner() -> Any:
    specification = importlib.util.spec_from_file_location(
        "w04_differential_test", RUNNER_PATH
    )
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


runner = load_runner()


class DifferentialTests(unittest.TestCase):
    def test_execution_comparison_is_byte_exact(self) -> None:
        reference = subprocess.CompletedProcess(
            ["reference"], 0, stdout=b"value\r\n", stderr=b""
        )
        candidate = subprocess.CompletedProcess(
            ["candidate"], 0, stdout=b"value\n", stderr=b""
        )
        self.assertEqual(runner.execution_differences(reference, candidate), ["stdout"])

    def test_execution_comparison_includes_termination_and_stderr(self) -> None:
        reference = subprocess.CompletedProcess(
            ["reference"], 0, stdout=b"", stderr=b"left"
        )
        candidate = subprocess.CompletedProcess(
            ["candidate"], -11, stdout=b"", stderr=b"right"
        )
        self.assertEqual(
            runner.execution_differences(reference, candidate),
            ["stderr", "termination"],
        )

    def test_cfg_minima_are_checked_without_a_golden_hash(self) -> None:
        case = {
            "expected_cfg": {
                "min_blocks": 2,
                "min_edges": 2,
                "min_phis": 1,
                "requires_backedge": True,
                "requires_loop": True,
            }
        }
        mir = (
            "znmir 1.3 module m0\n"
            "block b0 function f0 predecessors [b1] successors [b1]\n"
            "block b1 function f0 predecessors [b0] successors [b0]\n"
            "instruction i0 block b1 opcode phi representation i64 "
            "result v1 operands []\n"
        )
        self.assertEqual(runner.cfg_differences(case, mir, 1), [])
        self.assertNotIn("sha256", runner.cfg_differences.__doc__ or "")

    def test_self_test_exercises_corpus_and_all_calls(self) -> None:
        runner.self_test()

    def test_reference_and_candidate_must_be_explicit_and_distinct(self) -> None:
        with tempfile.TemporaryDirectory(prefix="w04-same-binary-") as directory:
            binary = Path(directory) / "php"
            binary.write_text("#!/bin/sh\nexit 0\n", encoding="ascii")
            binary.chmod(0o755)
            with self.assertRaisesRegex(runner.DifferentialError, "distinct"):
                runner.run(
                    str(binary),
                    str(binary),
                    runner.DEFAULT_MANIFEST,
                    5.0,
                )
            with self.assertRaisesRegex(runner.DifferentialError, "absolute"):
                runner.canonical_binary("php", "reference")

    def test_reference_and_candidate_hardlinks_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="w04-linked-binary-") as directory:
            binary = Path(directory) / "reference-php"
            linked = Path(directory) / "candidate-php"
            binary.write_text("#!/bin/sh\nexit 0\n", encoding="ascii")
            binary.chmod(0o755)
            os.link(binary, linked)
            with self.assertRaisesRegex(runner.DifferentialError, "distinct"):
                runner.run(
                    str(binary),
                    str(linked),
                    runner.DEFAULT_MANIFEST,
                    5.0,
                )


if __name__ == "__main__":
    unittest.main()
