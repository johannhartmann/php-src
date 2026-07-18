from pathlib import Path
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[4]
RUNNER = Path(__file__).with_name("run_scalar_mir_tests.py")


class ScalarMirTests(unittest.TestCase):
    def test_strict_scalar_mir_runner(self) -> None:
        completed = subprocess.run(
            [sys.executable, str(RUNNER), "--cc", "cc"],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertIn("W03 scalar MIR", completed.stdout)


if __name__ == "__main__":
    unittest.main()
