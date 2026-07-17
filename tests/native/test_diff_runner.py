#!/usr/bin/env python3
"""Self-tests for exact differential behavior and failure classification."""

from __future__ import annotations

import copy
import json
import os
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from typing import Any, Dict, Iterable

THIS_DIR = Path(__file__).resolve().parent
RUNNER = THIS_DIR / "diff_runner.py"
FIXTURES = THIS_DIR / "fixtures"
PHP = shutil.which("php")


WRAPPER = r'''#!/usr/bin/env python3
import os
import signal
import subprocess
import sys
import time

php = {php!r}
case = sys.argv[1]
name = os.path.basename(case)
if name == "stdout_diff.php":
    sys.stdout.write("candidate stdout\n")
elif name == "stderr_diff.php":
    sys.stderr.write("candidate stderr\n")
elif name == "exit_diff.php":
    sys.exit(7)
elif name == "signal_diff.php":
    os.kill(os.getpid(), signal.SIGTERM)
elif name == "timeout_diff.php":
    child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
    with open(os.path.join(os.path.dirname(case), "child.pid"), "w") as stream:
        stream.write(str(child.pid))
    time.sleep(30)
else:
    os.execv(php, [php, case])
'''


def scrub_volatile(document: Dict[str, Any]) -> Dict[str, Any]:
    value = copy.deepcopy(document)
    value.pop("generated_at_utc", None)
    for case in value.get("cases", []):
        for side in ("reference", "candidate"):
            case[side].pop("duration_ns", None)
            case[side]["stdout"].pop("artifact", None)
            case[side]["stderr"].pop("artifact", None)
    return value


@unittest.skipIf(PHP is None, "system PHP is required")
class DiffRunnerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="native diff tests ")
        self.root = Path(self.temporary.name)
        self.wrapper = self.root / "candidate wrapper"
        self.wrapper.write_text(WRAPPER.format(php=PHP), encoding="utf-8")
        self.wrapper.chmod(self.wrapper.stat().st_mode | stat.S_IXUSR)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def invoke(self, fixture: Path, candidate: str = PHP, name: str = "result.json", timeout: float = 2.0):
        output = self.root / name
        completed = subprocess.run(
            [
                sys.executable,
                str(RUNNER),
                "--reference",
                PHP,
                "--candidate",
                candidate,
                "--case",
                str(fixture),
                "--timeout",
                str(timeout),
                "--json-out",
                str(output),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=20,
        )
        return completed, json.loads(output.read_text(encoding="utf-8")), output

    def test_same_binary_is_equivalent_and_space_path_is_safe(self) -> None:
        completed, result, _ = self.invoke(FIXTURES / "path with spaces.php")
        self.assertEqual(0, completed.returncode, completed.stderr)
        self.assertEqual("equivalent", result["overall_status"])
        self.assertEqual("path with spaces.php", result["cases"][0]["case_id"])

    def test_stdout_difference(self) -> None:
        completed, result, _ = self.invoke(FIXTURES / "stdout_diff.php", str(self.wrapper))
        self.assertEqual(1, completed.returncode)
        self.assertEqual(["stdout"], result["cases"][0]["differences"])

    def test_stderr_difference(self) -> None:
        completed, result, _ = self.invoke(FIXTURES / "stderr_diff.php", str(self.wrapper))
        self.assertEqual(1, completed.returncode)
        self.assertEqual(["stderr"], result["cases"][0]["differences"])

    def test_exit_code_difference(self) -> None:
        completed, result, _ = self.invoke(FIXTURES / "exit_diff.php", str(self.wrapper))
        self.assertEqual(1, completed.returncode)
        self.assertEqual("exit", result["cases"][0]["candidate"]["termination"]["kind"])
        self.assertEqual(7, result["cases"][0]["candidate"]["termination"]["exit_code"])

    def test_signal_is_classified(self) -> None:
        completed, result, _ = self.invoke(FIXTURES / "signal_diff.php", str(self.wrapper))
        self.assertEqual(1, completed.returncode)
        termination = result["cases"][0]["candidate"]["termination"]
        self.assertEqual("signal", termination["kind"])
        self.assertEqual(signal.SIGTERM, termination["signal"])

    def test_timeout_kills_process_group(self) -> None:
        copied = self.root / "timeout_diff.php"
        shutil.copyfile(str(FIXTURES / "timeout_diff.php"), str(copied))
        completed, result, _ = self.invoke(copied, str(self.wrapper), timeout=0.25)
        self.assertEqual(3, completed.returncode)
        self.assertEqual("TIMEOUT", result["cases"][0]["status"])
        pid = int((self.root / "child.pid").read_text(encoding="ascii"))
        for _ in range(50):
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                break
            time.sleep(0.02)
        else:
            self.fail("grandchild process {} survived process-group timeout".format(pid))

    def test_binary_output_is_preserved(self) -> None:
        completed, result, output = self.invoke(FIXTURES / "binary_output.php")
        self.assertEqual(0, completed.returncode)
        case = result["cases"][0]
        stdout_path = output.parent / case["reference"]["stdout"]["artifact"]
        stderr_path = output.parent / case["reference"]["stderr"]["artifact"]
        self.assertEqual(b"prefix\x00suffix\xff", stdout_path.read_bytes())
        self.assertEqual(b"error\x00bytes", stderr_path.read_bytes())

    def test_large_output_is_streamed_to_artifacts(self) -> None:
        completed, result, output = self.invoke(FIXTURES / "large_output.php", timeout=5.0)
        self.assertEqual(0, completed.returncode)
        stream = result["cases"][0]["reference"]["stdout"]
        self.assertEqual(5 * 1024 * 1024, stream["length"])
        self.assertEqual(stream["sha256"], result["cases"][0]["candidate"]["stdout"]["sha256"])

    def test_directory_sort_and_case_ids_are_stable(self) -> None:
        cases = self.root / "ordered cases"
        cases.mkdir()
        shutil.copyfile(str(FIXTURES / "equivalent.php"), str(cases / "z.php"))
        nested = cases / "nested"
        nested.mkdir()
        shutil.copyfile(str(FIXTURES / "equivalent.php"), str(nested / "a.php"))
        completed, result, _ = self.invoke(cases)
        self.assertEqual(0, completed.returncode)
        self.assertEqual(["nested/a.php", "z.php"], [case["case_id"] for case in result["cases"]])

    def test_repeat_is_semantically_stable_except_declared_volatility(self) -> None:
        first_completed, first, _ = self.invoke(FIXTURES / "equivalent.php", name="first.json")
        second_completed, second, _ = self.invoke(FIXTURES / "equivalent.php", name="second.json")
        self.assertEqual(0, first_completed.returncode)
        self.assertEqual(0, second_completed.returncode)
        self.assertEqual(first["volatile_fields"], second["volatile_fields"])
        self.assertEqual(scrub_volatile(first), scrub_volatile(second))

    def test_harness_error_has_distinct_exit_code(self) -> None:
        completed, result, _ = self.invoke(self.root / "missing.php")
        self.assertEqual(2, completed.returncode)
        self.assertEqual("harness_error", result["overall_status"])


if __name__ == "__main__":
    unittest.main()
