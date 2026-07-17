"""Contract tests for explicit W01 semantic reference capture."""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SEMANTICS_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SEMANTICS_ROOT))

import capture_reference as capture_tool  # noqa: E402


class ReferenceCaptureTests(unittest.TestCase):
    def test_deterministic_environment_does_not_inherit_php_configuration(self) -> None:
        reference = Path("/absolute/reference/php")
        config_dir = Path("/absolute/empty-config")
        with mock.patch.dict(os.environ, {
            "PHPRC": "/hostile/php.ini",
            "PHP_INI_SCAN_DIR": "/hostile/conf.d",
            "TEST_PHP_ARGS": "-d auto_prepend_file=/hostile/prepend.php",
        }):
            environment = capture_tool.deterministic_environment(reference, config_dir)
        self.assertEqual(str(config_dir), environment["PHPRC"])
        self.assertEqual("", environment["PHP_INI_SCAN_DIR"])
        self.assertNotIn("TEST_PHP_ARGS", environment)

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
        with tempfile.TemporaryDirectory(prefix="w01-e-hostile-") as hostile_directory, \
                tempfile.TemporaryDirectory(prefix="w01-e-reference-") as temporary_directory:
            hostile_ini = Path(hostile_directory) / "php.ini"
            hostile_ini.write_text("auto_prepend_file=/does/not/exist.php\n", encoding="utf-8")
            with mock.patch.dict(os.environ, {"PHPRC": str(hostile_ini)}):
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
            expected_inputs = {
                fixture["source_path"]
                for fixture in json.loads(capture_tool.MANIFEST_PATH.read_text(encoding="utf-8"))["fixtures"]
            }
            expected_inputs.update(
                path.relative_to(capture_tool.REPOSITORY_ROOT).as_posix()
                for path in (capture_tool.REPOSITORY_ROOT / "tests/native/semantics/corpus").rglob("*")
                if path.is_file()
            )
            indexed_inputs = {item["path"] for item in index["inputs"]}
            self.assertEqual(expected_inputs, indexed_inputs)
            self.assertTrue(all(len(item["sha256"]) == 64 for item in index["inputs"]))
            with (bundle.parent / "phpt-result.json").open("r", encoding="utf-8") as result_file:
                phpt_result = json.load(result_file)
            self.assertNotEqual(str(hostile_ini), phpt_result["deterministic_environment"]["PHPRC"])


if __name__ == "__main__":
    unittest.main()
