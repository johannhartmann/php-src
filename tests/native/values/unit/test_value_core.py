from __future__ import annotations

import os
import subprocess
import unittest
from pathlib import Path


class ValueCoreTest(unittest.TestCase):
    def test_standalone_core_suite(self) -> None:
        script = Path(__file__).with_name("run_value_core_tests.py")
        subprocess.run(
            ["python3", str(script), "--cc", os.environ.get("CC", "cc")],
            check=True,
        )


if __name__ == "__main__":
    unittest.main()
