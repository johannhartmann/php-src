"""Structural checks for W01-E PHPT and repeated-call fixtures."""

from __future__ import annotations

import json
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[5]
SEMANTICS_ROOT = REPOSITORY_ROOT / "tests" / "native" / "semantics"


class CorpusContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        with (SEMANTICS_ROOT / "manifest.json").open("r", encoding="utf-8") as manifest_file:
            cls.manifest = json.load(manifest_file)

    def test_new_phpts_have_minimum_sections(self) -> None:
        for fixture in self.manifest["fixtures"]:
            if fixture["kind"] != "new_phpt":
                continue
            with self.subTest(fixture=fixture["fixture_id"]):
                contents = (REPOSITORY_ROOT / fixture["source_path"]).read_text(encoding="utf-8")
                self.assertIn("--TEST--\n", contents)
                self.assertIn("--FILE--\n", contents)
                self.assertTrue("--EXPECT--\n" in contents or "--EXPECTF--\n" in contents)

    def test_zend_test_fixtures_declare_extension(self) -> None:
        for fixture in self.manifest["fixtures"]:
            if fixture["kind"] != "new_phpt" or "zend_test" not in fixture["required_extensions"]:
                continue
            with self.subTest(fixture=fixture["fixture_id"]):
                contents = (REPOSITORY_ROOT / fixture["source_path"]).read_text(encoding="utf-8")
                self.assertIn("--EXTENSIONS--\nzend_test\n", contents)

    def test_calls_one_through_ten_have_distinct_cases(self) -> None:
        calls = {}
        for fixture in self.manifest["fixtures"]:
            if fixture["fixture_id"].startswith("calls_repeat_"):
                self.assertEqual(1, len(fixture["repeat_calls"]))
                calls[fixture["repeat_calls"][0]] = fixture["source_path"]
        self.assertEqual(list(range(1, 11)), sorted(calls))
        self.assertEqual(10, len(set(calls.values())))
        for call, source_path in calls.items():
            with self.subTest(call=call):
                contents = (REPOSITORY_ROOT / source_path).read_text(encoding="utf-8")
                self.assertIn("$call <= {}".format(call), contents)
                self.assertIn("call-{:02d}".format(call), contents)

    def test_bailout_fixture_calls_proven_test_extension_api(self) -> None:
        fixture = next(item for item in self.manifest["fixtures"] if item["fixture_id"] == "bailout_nonlocal_transfer")
        contents = (REPOSITORY_ROOT / fixture["source_path"]).read_text(encoding="utf-8")
        self.assertIn("zend_trigger_bailout();", contents)
        self.assertIn("zend_test", fixture["required_extensions"])


if __name__ == "__main__":
    unittest.main()
