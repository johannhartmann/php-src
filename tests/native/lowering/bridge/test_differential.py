"""Unit tests for the W03 differential harness."""

from __future__ import annotations

import importlib.util
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[4]
RUNNER_PATH = ROOT / "scripts/native/lowering/run-w03-differential.py"


def load_runner() -> Any:
    specification = importlib.util.spec_from_file_location(
        "w03_differential_test", RUNNER_PATH
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
        self.assertEqual(
            runner.execution_differences(reference, candidate),
            ["stdout"],
        )

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

    def test_self_test_exercises_all_calls_and_injection_safety(self) -> None:
        runner.self_test()

    def test_reference_and_candidate_must_be_distinct_paths(self) -> None:
        with tempfile.TemporaryDirectory(prefix="w03-same-binary-") as directory:
            binary = Path(directory) / "php"
            binary.write_text("#!/bin/sh\nexit 0\n", encoding="ascii")
            binary.chmod(0o755)
            with self.assertRaisesRegex(runner.DifferentialError, "distinct"):
                runner.run(str(binary), str(binary), Path("missing.json"), 5.0)

    def test_reference_and_candidate_hardlinks_must_be_distinct(self) -> None:
        with tempfile.TemporaryDirectory(prefix="w03-linked-binary-") as directory:
            binary = Path(directory) / "reference-php"
            linked = Path(directory) / "candidate-php"
            binary.write_text("#!/bin/sh\nexit 0\n", encoding="ascii")
            binary.chmod(0o755)
            os.link(binary, linked)
            with self.assertRaisesRegex(runner.DifferentialError, "distinct"):
                runner.run(str(binary), str(linked), Path("missing.json"), 5.0)


if __name__ == "__main__":
    unittest.main()
