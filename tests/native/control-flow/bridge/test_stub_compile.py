"""Compile and execute the W04-B failure-atomic entry stub."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
BRIDGE = ROOT / "tests/native/control-flow/bridge"
STUB = BRIDGE / "w04_entry_stub.c"
HARNESS = BRIDGE / "test_w04_entry_stub.c"


class StubCompileTests(unittest.TestCase):
    def compile_and_run(self, compiler: str, standard: str, language: str) -> None:
        executable_name = shutil.which(compiler)
        if executable_name is None:
            self.skipTest("{} is unavailable".format(compiler))
        with tempfile.TemporaryDirectory(prefix="w04-entry-stub-") as directory:
            executable = Path(directory) / "stub-test"
            command = [
                executable_name,
                "-x",
                language,
                "-std={}".format(standard),
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT),
                str(STUB),
                str(HARNESS),
                "-o",
                str(executable),
            ]
            completed = subprocess.run(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
                env={**os.environ, "LANG": "C", "LC_ALL": "C"},
            )
            self.assertEqual(
                completed.returncode,
                0,
                "{}\n{}".format(completed.stdout, completed.stderr),
            )
            executed = subprocess.run(
                [str(executable)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
            )
            self.assertEqual(executed.returncode, 0, executed.stderr)
            self.assertEqual(executed.stdout, "")
            self.assertEqual(executed.stderr, "")

    def test_stub_is_failure_atomic_in_c11(self) -> None:
        self.compile_and_run(os.environ.get("CC", "cc"), "c11", "c")

    def test_stub_is_failure_atomic_in_cpp20(self) -> None:
        self.compile_and_run(os.environ.get("CXX", "c++"), "c++20", "c++")


if __name__ == "__main__":
    unittest.main()
