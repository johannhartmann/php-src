"""Static and deterministic tests for the W03 integration gate."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import unittest
from typing import Any


ROOT = Path(__file__).resolve().parents[4]
VALIDATOR_PATH = ROOT / "scripts/native/lowering/validate-w03.py"
DUMP_PATH = ROOT / "scripts/native/lowering/dump-mir.py"
EXTENSION_PATH = ROOT / "ext/native_mir_test/native_mir_test.c"
WORKFLOW_PATH = ROOT / ".github/workflows/native-w03.yml"


def load_module(name: str, path: Path) -> Any:
    specification = importlib.util.spec_from_file_location(
        name, path
    )
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


validator = load_module("w03_integration_validator", VALIDATOR_PATH)
dump_mir = load_module("w03_integration_dump", DUMP_PATH)


class W03IntegrationGateTests(unittest.TestCase):
    def test_coverage_report_is_current_and_timestamp_free(self) -> None:
        expected = validator.report_bytes()
        actual = (
            ROOT / "docs/native-engine/lowering/w03-coverage-report.json"
        ).read_bytes()
        self.assertEqual(actual, expected)
        document = json.loads(actual)
        self.assertNotIn("timestamp", document)
        self.assertFalse(document["determinism"]["report_has_timestamp"])

    def test_profile_has_exact_provider_and_proof_coverage(self) -> None:
        document = validator.report_document()
        self.assertEqual(document["profile"]["accepted_opcode_count"], 25)
        self.assertEqual(document["profile"]["deferred_opcode_count"], 187)
        self.assertTrue(
            all(
                item["status"] in {"covered", "not-required"}
                for item in document["proof_coverage"].values()
            )
        )
        claimed = sum(
            item["claimed_opcode_count"]
            for item in document["provider_coverage"].values()
        )
        self.assertEqual(claimed, document["profile"]["accepted_opcode_count"])

    def test_test_bridge_exposes_arena_determinism_without_runtime_activation(
        self,
    ) -> None:
        extension = EXTENSION_PATH.read_text(encoding="utf-8")
        self.assertIn('"arena_chunk_size"', extension)
        self.assertIn("state->mir_chunk_size", extension)
        self.assertNotIn("zend_execute(", extension)
        config = (ROOT / "ext/native_mir_test/config.m4").read_text(
            encoding="utf-8"
        )
        self.assertRegex(config, r"\[no\],\s*\[no\]\)")

    def test_w03_specific_profiles_are_explicit_and_test_only(self) -> None:
        profiles = {
            "w03-debug-nts": ("nts", "none"),
            "w03-debug-zts": ("zts", "none"),
            "w03-asan-nts": ("nts", "address"),
            "w03-ubsan-nts": ("nts", "undefined"),
        }
        for name, (thread_safety, sanitizer) in profiles.items():
            with self.subTest(profile=name):
                text = (
                    ROOT / f"scripts/native/profiles/{name}.env"
                ).read_text(encoding="utf-8")
                self.assertIn("--enable-native-mir-test", text)
                self.assertIn(
                    f"PROFILE_THREAD_SAFETY={thread_safety}", text
                )
                self.assertIn(f"PROFILE_SANITIZER={sanitizer}", text)

    def test_determinism_subprocess_environment_preserves_sanitizer_policy(
        self,
    ) -> None:
        strict = {
            "ASAN_OPTIONS": "abort_on_error=1:detect_leaks=1",
            "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
        }
        environment = dump_mir.deterministic_environment(strict)
        self.assertEqual(environment["ASAN_OPTIONS"], strict["ASAN_OPTIONS"])
        self.assertEqual(environment["UBSAN_OPTIONS"], strict["UBSAN_OPTIONS"])
        self.assertEqual(environment["LC_ALL"], "C")
        source = DUMP_PATH.read_text(encoding="utf-8")
        self.assertIn("env=deterministic_environment(environment)", source)

    def test_workflow_preserves_complete_external_gate_evidence(self) -> None:
        workflow = WORKFLOW_PATH.read_text(encoding="utf-8")
        self.assertIn("cat \"$build_log\" >>", workflow)
        self.assertIn("actions/upload-artifact@v6", workflow)
        self.assertIn("${{ runner.temp }}/wave-results", workflow)
        self.assertIn("--output \"$RUNNER_TEMP/w03-artifacts/status.md\"", workflow)
        gate = (ROOT / "scripts/native/lowering/test-w03.py").read_text(
            encoding="utf-8"
        )
        self.assertIn("detect_leaks=1", gate)

    def test_fixed_seed_fuzz_smoke(self) -> None:
        completed = subprocess.run(
            [
                sys.executable,
                "tests/native/lowering/fuzz/run_fuzz.py",
                "--seed",
                "20260718",
                "--cases",
                "300",
            ],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(json.loads(completed.stdout)["status"], "pass")


if __name__ == "__main__":
    unittest.main()
