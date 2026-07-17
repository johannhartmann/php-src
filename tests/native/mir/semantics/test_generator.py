"""Drift and determinism tests for the W02 semantic catalog generator."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[4]
GENERATOR = REPO_ROOT / "scripts/native/mir/generate-semantic-ids.py"
MODEL = REPO_ROOT / "docs/native-engine/semantics/effects/effect-model.json"
HEADER = REPO_ROOT / "Zend/Native/MIR/zend_mir_effects.h"
OUTPUT = REPO_ROOT / "Zend/Native/MIR/Semantics/zend_mir_semantic_catalog.inc"


class GeneratorTest(unittest.TestCase):
    def run_generator(self, *arguments: str, check: bool = True) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(GENERATOR), *arguments], cwd=REPO_ROOT,
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=check,
        )

    def test_checked_in_catalog_is_current(self) -> None:
        result = self.run_generator("--check")
        self.assertIn("matches the frozen W01 model", result.stdout)

    def test_generation_is_deterministic(self) -> None:
        with tempfile.TemporaryDirectory(prefix="zend-mir-generator-") as directory:
            first = Path(directory) / "first.inc"
            second = Path(directory) / "second.inc"
            self.run_generator("--output", str(first))
            self.run_generator("--output", str(second))
            self.assertEqual(first.read_bytes(), second.read_bytes())
            self.assertEqual(first.read_bytes(), OUTPUT.read_bytes())

    def test_catalog_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="zend-mir-drift-") as directory:
            model_path = Path(directory) / "effect-model.json"
            output_path = Path(directory) / "catalog.inc"
            model = json.loads(MODEL.read_text(encoding="utf-8"))
            model["catalog"]["effects"][0] = "drifted_read"
            model_path.write_text(json.dumps(model), encoding="utf-8")
            result = self.run_generator(
                "--model", str(model_path), "--header", str(HEADER),
                "--output", str(output_path), check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("differs from frozen", result.stderr)
            self.assertFalse(output_path.exists())

    def test_check_detects_stale_output(self) -> None:
        with tempfile.TemporaryDirectory(prefix="zend-mir-stale-") as directory:
            output_path = Path(directory) / "catalog.inc"
            output_path.write_text("stale\n", encoding="utf-8")
            result = self.run_generator("--output", str(output_path), "--check", check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("stale", result.stderr)


if __name__ == "__main__":
    unittest.main()
