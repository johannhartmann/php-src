"""Integration test for the standalone W02-A test driver."""

from __future__ import annotations

import os
import subprocess
import sys
import unittest
from pathlib import Path


class CoreTestDriverTest(unittest.TestCase):
    def test_driver_compiles_and_passes(self) -> None:
        driver = Path(__file__).with_name("run_core_tests.py")
        compiler = os.environ.get("CC", "cc")
        result = subprocess.run(
            [sys.executable, str(driver), "--cc", compiler],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("core MIR tests passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
