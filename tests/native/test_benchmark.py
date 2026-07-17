#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

THIS_DIR = Path(__file__).resolve().parent
ROOT = THIS_DIR.parents[1]
BENCH_DIR = ROOT / "bench" / "native"
RUNNER = BENCH_DIR / "benchmark_runner.py"
SCENARIOS = BENCH_DIR / "scenarios"
PHP = shutil.which("php")

FAKE_PHP = r'''#!/usr/bin/env python3
import json
import sys

if "-r" in sys.argv:
    print(json.dumps({
        "architecture": "x86_64",
        "debug": False,
        "integer_size_bytes": 8,
        "opcache_enable_cli": "0",
        "opcache_loaded": False,
        "os_family": "Linux",
        "php_binary": sys.argv[0],
        "php_sapi": "cli",
        "php_version": "0.0.0-test",
        "php_version_id": 0,
        "thread_safety": "NTS",
        "zend_version": "0.0.0-test",
    }))
elif "-i" in sys.argv:
    print("Server API => Command Line Interface")
    print("Debug Build => no")
    print("Thread Safety => disabled")
elif "-v" in sys.argv:
    print("PHP 0.0.0-test (cli)")
else:
    sys.exit(2)
'''

spec = importlib.util.spec_from_file_location("benchmark_runner", RUNNER)
benchmark_runner = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(benchmark_runner)


@unittest.skipIf(PHP is None, "system PHP is required")
class BenchmarkTests(unittest.TestCase):
    def test_descriptors_cover_required_scenario_classes(self) -> None:
        descriptors = [benchmark_runner.validate_descriptor(path) for path in sorted(SCENARIOS.glob("*.json"))]
        self.assertEqual(8, len(descriptors))
        self.assertEqual(8, len({item["scenario_id"] for item in descriptors}))

    def test_local_run_has_ten_calls_and_does_not_promote(self) -> None:
        with tempfile.TemporaryDirectory(prefix="native benchmark tests ") as temporary:
            output = Path(temporary) / "local output"
            baseline_entries_before = set((BENCH_DIR / "baselines").glob("*")) if (BENCH_DIR / "baselines").exists() else set()
            completed = subprocess.run(
                [
                    sys.executable,
                    str(RUNNER),
                    "--php",
                    PHP,
                    "--scenario",
                    str(SCENARIOS / "01-arithmetic.json"),
                    "--output-dir",
                    str(output),
                    "--repetitions",
                    "2",
                    "--steady-state-calls",
                    "2",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=30,
            )
            self.assertEqual(0, completed.returncode, completed.stderr.decode())
            result = json.loads((output / "benchmark-result.json").read_text(encoding="utf-8"))
            benchmark_runner.validate_benchmark_result(result)
            self.assertEqual(2, len(result["scenarios"][0]["samples"]))
            self.assertTrue(all(len(sample["call_ns"]) == 10 for sample in result["scenarios"][0]["samples"]))
            self.assertTrue(all(len(sample["steady_state_ns"]) == 2 for sample in result["scenarios"][0]["samples"]))
            baseline_entries_after = set((BENCH_DIR / "baselines").glob("*")) if (BENCH_DIR / "baselines").exists() else set()
            self.assertEqual(baseline_entries_before, baseline_entries_after)

    def test_promotion_rejects_unknown_dirty_and_mismatched_provenance(self) -> None:
        manifest = {"provenance": {"commit": None, "dirty": None, "reason": "unknown"}}
        with self.assertRaisesRegex(ValueError, "unknown repository commit"):
            benchmark_runner.validate_promotion_manifest(manifest, "a" * 40)
        manifest["provenance"] = {"commit": "a" * 40, "dirty": True, "reason": None}
        with self.assertRaisesRegex(ValueError, "dirty"):
            benchmark_runner.validate_promotion_manifest(manifest, "a" * 40)
        manifest["provenance"] = {"commit": "a" * 40, "dirty": False, "reason": None}
        with self.assertRaisesRegex(ValueError, "does not match"):
            benchmark_runner.validate_promotion_manifest(manifest, "b" * 40)

    def test_cli_promotion_with_system_php_is_refused_before_writing(self) -> None:
        baseline_id = "selftest-must-not-exist"
        destination = BENCH_DIR / "baselines" / baseline_id
        self.assertFalse(destination.exists())
        completed = subprocess.run(
            [
                sys.executable,
                str(RUNNER),
                "--php",
                PHP,
                "--scenario",
                str(SCENARIOS / "01-arithmetic.json"),
                "--promote-baseline",
                baseline_id,
                "--expected-commit",
                "a" * 40,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=20,
        )
        self.assertEqual(2, completed.returncode)
        self.assertFalse(destination.exists())

    def test_cli_promotion_with_dirty_binary_worktree_is_refused(self) -> None:
        baseline_id = "selftest-dirty-must-not-exist"
        destination = BENCH_DIR / "baselines" / baseline_id
        self.assertFalse(destination.exists())
        with tempfile.TemporaryDirectory(prefix="native dirty binary ") as temporary:
            repository = Path(temporary)
            fake_php = repository / "php"
            fake_php.write_text(FAKE_PHP, encoding="utf-8")
            fake_php.chmod(0o755)
            subprocess.run(["git", "init", "-q", str(repository)], check=True)
            subprocess.run(["git", "-C", str(repository), "add", "php"], check=True)
            subprocess.run(
                ["git", "-C", str(repository), "-c", "user.name=W00 Test", "-c", "user.email=w00@example.invalid", "commit", "-q", "-m", "fixture"],
                check=True,
            )
            commit = subprocess.check_output(["git", "-C", str(repository), "rev-parse", "HEAD"], text=True).strip()
            with fake_php.open("a", encoding="utf-8") as stream:
                stream.write("\n# dirty\n")
            completed = subprocess.run(
                [
                    sys.executable,
                    str(RUNNER),
                    "--php",
                    str(fake_php),
                    "--scenario",
                    str(SCENARIOS / "01-arithmetic.json"),
                    "--promote-baseline",
                    baseline_id,
                    "--expected-commit",
                    commit,
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=20,
            )
        self.assertEqual(2, completed.returncode)
        self.assertIn(b"dirty", completed.stderr)
        self.assertFalse(destination.exists())


if __name__ == "__main__":
    unittest.main()
