import os
from pathlib import Path
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[4]


class TextFormatTest(unittest.TestCase):
    def test_complete_text_harness(self) -> None:
        subprocess.run(
            [
                sys.executable,
                "tests/native/mir/text/run_text_tests.py",
                "--cc",
                os.environ.get("CC", "cc"),
            ],
            cwd=ROOT,
            check=True,
            timeout=90,
        )


if __name__ == "__main__":
    unittest.main()
