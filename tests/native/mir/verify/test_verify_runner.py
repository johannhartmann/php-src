"""Black-box checks for the standalone W02-F verifier test driver."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import unittest


TEST_DIRECTORY = Path(__file__).resolve().parent
REPOSITORY_ROOT = TEST_DIRECTORY.parents[3]


class VerifyRunnerTest(unittest.TestCase):
    def test_strict_build_runtime_and_public_header(self) -> None:
        environment = os.environ.copy()
        environment.update({"LC_ALL": "C", "TZ": "UTC"})
        completed = subprocess.run(
            [
                sys.executable,
                str(TEST_DIRECTORY / "run_verify_tests.py"),
                "--cc",
                environment.get("CC", "cc"),
            ],
            cwd=REPOSITORY_ROOT,
            env=environment,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertIn("verify tests: ok", completed.stdout)
        self.assertIn("W02-F verifier tests passed", completed.stdout)

    def test_required_passes_and_diagnostic_families_are_covered(self) -> None:
        verify_directory = (
            REPOSITORY_ROOT / "Zend/Native/MIR/Verify"
        )
        required_passes = {
            "zend_mir_verify_ids.c",
            "zend_mir_verify_cfg.c",
            "zend_mir_verify_dominance.c",
            "zend_mir_verify_semantics.c",
            "zend_mir_verify_frames.c",
        }
        self.assertEqual(
            required_passes,
            {path.name for path in verify_directory.glob("zend_mir_verify_*.c")}
            - {"zend_mir_verify.c"},
        )

        test_source = (TEST_DIRECTORY / "test_verify.c").read_text(
            encoding="utf-8"
        )
        for family in ("MIRV01", "MIRV02", "MIRV03", "MIRV04", "MIRV05"):
            self.assertIn(family, test_source)
        for required_case in (
            "make_double_destroy",
            "test_determinism_and_negative_nonmutation",
            "zend_mir_verify_test_fail_allocation_after",
        ):
            self.assertIn(required_case, test_source)


if __name__ == "__main__":
    unittest.main()
