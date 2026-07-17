"""Contract tests for explicit W01 semantic reference capture."""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path


SEMANTICS_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SEMANTICS_ROOT))

import capture_reference as capture_tool  # noqa: E402


class ReferenceCaptureTests(unittest.TestCase):
    def test_relative_reference_binary_is_rejected_without_path_search(self) -> None:
        with self.assertRaisesRegex(capture_tool.CaptureError, "explicit absolute path"):
            capture_tool.explicit_executable("php")

    def test_repository_artifact_directory_is_rejected(self) -> None:
        artifact_dir = capture_tool.REPOSITORY_ROOT / "tests" / "native" / "semantics" / "artifacts"
        with self.assertRaisesRegex(capture_tool.CaptureError, "outside the repository"):
            capture_tool.external_empty_artifact_dir(artifact_dir)

    def test_nonempty_artifact_directory_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            artifact_dir = Path(temporary_directory)
            (artifact_dir / "existing").write_text("do not overwrite", encoding="utf-8")
            with self.assertRaisesRegex(capture_tool.CaptureError, "must be empty"):
                capture_tool.external_empty_artifact_dir(artifact_dir)

    def test_reference_self_diff_with_explicit_binary(self) -> None:
        reference_value = os.environ.get("W01_E_REFERENCE_PHP")
        if not reference_value:
            self.skipTest("W01_E_REFERENCE_PHP is not explicitly set")
        reference = Path(reference_value)
        if not reference.is_absolute():
            self.fail("W01_E_REFERENCE_PHP must be absolute")
        with tempfile.TemporaryDirectory(prefix="w01-e-reference-") as temporary_directory:
            bundle = capture_tool.capture(str(reference), Path(temporary_directory))
            with bundle.open("r", encoding="utf-8") as bundle_file:
                index = json.load(bundle_file)
            self.assertEqual("w01_semantic_reference_bundle", index["result_type"])
            self.assertIn("not native equivalence", index["purpose"])
            self.assertTrue((bundle.parent / "reference-binary-manifest.json").is_file())
            with (bundle.parent / "reference-self-diff.json").open("r", encoding="utf-8") as result_file:
                result = json.load(result_file)
            self.assertEqual("equivalent", result["overall_status"])
            self.assertEqual(11, result["summary"]["total"])


if __name__ == "__main__":
    unittest.main()
