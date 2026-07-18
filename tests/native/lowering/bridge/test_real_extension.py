"""Runtime tests for the default-off W03 compile/dump extension."""

from __future__ import annotations

import importlib.util
import os
import tempfile
import unittest
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[4]
DUMP_PATH = ROOT / "scripts/native/lowering/dump-mir.py"
CANDIDATE_VALUE = os.environ.get("W03_CANDIDATE_PHP")


def load_dump_module() -> Any:
    specification = importlib.util.spec_from_file_location(
        "w03_real_extension_dump", DUMP_PATH
    )
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


dump_mir = load_dump_module()


@unittest.skipUnless(
    CANDIDATE_VALUE,
    "set W03_CANDIDATE_PHP to the explicit test-build PHP binary",
)
class RealExtensionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        assert CANDIDATE_VALUE is not None
        cls.candidate = str(dump_mir.canonical_executable(CANDIDATE_VALUE))
        cls.temporary_directory = tempfile.TemporaryDirectory(
            prefix="w03-never-execute-"
        )
        cls.sentinel = Path(cls.temporary_directory.name) / "source-executed"
        php_sentinel = (
            str(cls.sentinel).replace("\\", "\\\\").replace("'", "\\'").encode()
        )
        cls.source = (
            b"<?php\n"
            + b"file_put_contents('" + php_sentinel + b"', 'executed');\n"
            + b"function w03_never_execute(int $value): int {\n"
            + b"    return $value | 1;\n"
            + b"}\n"
        )
        cls.filename = "w03-never-execute.php"

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary_directory.cleanup()

    def setUp(self) -> None:
        self.sentinel.unlink(missing_ok=True)

    def tearDown(self) -> None:
        self.sentinel.unlink(missing_ok=True)

    def test_source_is_not_executed_and_calls_one_through_ten_are_exact(self) -> None:
        document, exit_code = dump_mir.invoke(
            self.candidate,
            self.source,
            self.filename,
            function="w03_never_execute",
            repeat=10,
        )
        self.assertEqual(exit_code, dump_mir.EXIT_ACCEPTED)
        self.assertEqual(
            [call["call"] for call in document["calls"]],
            list(range(1, 11)),
        )
        self.assertEqual(
            {call["mir_sha256"] for call in document["calls"]},
            {document["calls"][0]["mir_sha256"]},
        )
        self.assertFalse(self.sentinel.exists())

        # A fresh process exercises extension shutdown after repeated calls.
        second, second_exit = dump_mir.invoke(
            self.candidate,
            self.source,
            self.filename,
            function="w03_never_execute",
        )
        self.assertEqual(second_exit, dump_mir.EXIT_ACCEPTED)
        self.assertEqual(
            second["calls"][0]["mir_sha256"],
            document["calls"][0]["mir_sha256"],
        )
        self.assertFalse(self.sentinel.exists())

    def test_compile_error_is_structured_and_repeatable(self) -> None:
        document, exit_code = dump_mir.invoke(
            self.candidate,
            b"<?php function broken( {",
            "invalid-w03.php",
            function="broken",
            repeat=3,
        )
        self.assertEqual(exit_code, dump_mir.EXIT_ERROR)
        self.assertEqual(document["status"], "error")
        self.assertEqual(document["calls"][0]["result"]["phase"], "compile")
        self.assertIn(
            "COMPILE_ERROR",
            {
                diagnostic["code"]
                for diagnostic in document["calls"][0]["result"]["diagnostics"]
            },
        )

    def test_bailout_and_fault_boundaries_are_failure_atomic(self) -> None:
        cases = {
            "compile_bailout": ("error", "compile", "BAILOUT"),
            "ssa_failure": ("rejected", "ssa", "SSA0001"),
            "lower_failure": ("error", "lowering", "MIRL0007"),
            "module_oom": ("error", "lowering", "MIRL0001"),
            "dump_failure": ("error", "dump", "MIRV0011"),
        }
        for fault, (status, phase, code) in cases.items():
            with self.subTest(fault=fault):
                document, _ = dump_mir.invoke(
                    self.candidate,
                    self.source,
                    self.filename,
                    function="w03_never_execute",
                    repeat=3,
                    fault=fault,
                )
                result = document["calls"][0]["result"]
                self.assertEqual(result["status"], status)
                self.assertEqual(result["phase"], phase)
                self.assertIsNone(result["mir"])
                self.assertIn(
                    code,
                    {diagnostic["code"] for diagnostic in result["diagnostics"]},
                )
                self.assertFalse(self.sentinel.exists())

    def test_missing_function_does_not_publish_partial_mir(self) -> None:
        document, exit_code = dump_mir.invoke(
            self.candidate,
            self.source,
            self.filename,
            function="missing_function",
            repeat=3,
        )
        self.assertEqual(exit_code, dump_mir.EXIT_REJECTED)
        result = document["calls"][0]["result"]
        self.assertEqual(result["status"], "rejected")
        self.assertEqual(result["phase"], "compile")
        self.assertIsNone(result["mir"])
        self.assertIn(
            "MIRL0006",
            {diagnostic["code"] for diagnostic in result["diagnostics"]},
        )
        self.assertFalse(self.sentinel.exists())


if __name__ == "__main__":
    unittest.main()
