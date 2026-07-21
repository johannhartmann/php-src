"""Static and deterministic W03 integration tests."""

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


class W03IntegrationTests(unittest.TestCase):
    def test_profile_has_exact_provider_and_proof_coverage(self) -> None:
        profile = json.loads(
            (ROOT / "docs/native-engine/lowering/w03-opcode-profile.json").read_text(
                encoding="utf-8"
            )
        )
        validator.validate_profile(profile)
        accepted = [
            entry for entry in profile["opcodes"]
            if entry["classification"] != "deferred"
        ]
        deferred = [
            entry for entry in profile["opcodes"]
            if entry["classification"] == "deferred"
        ]
        self.assertEqual(25, len(accepted))
        self.assertEqual(187, len(deferred))
        self.assertEqual(
            set(validator.PROVIDER_PATHS),
            {entry["provider"] for entry in accepted},
        )

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
            timeout=120,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(json.loads(completed.stdout)["status"], "pass")

    def test_lowering_harness_self_test(self) -> None:
        completed = subprocess.run(
            [
                sys.executable,
                "scripts/native/lowering/test-w03.py",
                "--self-test",
            ],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
            timeout=90,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn(
            "W03 lowering harness self-test passed", completed.stdout
        )


if __name__ == "__main__":
    unittest.main()
