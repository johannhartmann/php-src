#!/usr/bin/env python3
"""Black-box checks for the standalone CFG test runner."""

import os
from pathlib import Path
import subprocess
import sys
import unittest


TEST_DIRECTORY = Path(__file__).resolve().parent
REPOSITORY_ROOT = TEST_DIRECTORY.parents[3]


class CfgRunnerTest(unittest.TestCase):
    def test_strict_build_and_runtime_suite(self) -> None:
        environment = os.environ.copy()
        environment.update({"LC_ALL": "C", "TZ": "UTC"})
        command = [
            sys.executable,
            str(TEST_DIRECTORY / "run_cfg_tests.py"),
            "--cc",
            environment.get("CC", "cc"),
            "--cxx",
            environment.get("CXX", "c++"),
        ]
        completed = subprocess.run(
            command,
            cwd=REPOSITORY_ROOT,
            env=environment,
            capture_output=True,
            text=True,
        )
        if completed.returncode != 0:
            self.fail(
                f"CFG runner exited with {completed.returncode}\n"
                f"stdout:\n{completed.stdout}\n"
                f"stderr:\n{completed.stderr}"
            )
        self.assertIn("CFG tests passed", completed.stdout)


if __name__ == "__main__":
    unittest.main()
