#!/usr/bin/env python3

from __future__ import annotations

import json
import unittest
from pathlib import Path

from schema_validation import validate_document


THIS_DIR = Path(__file__).resolve().parent
ROOT = THIS_DIR.parents[1]


class ContractTests(unittest.TestCase):
    def test_every_schema_is_valid_json_with_draft_and_id(self) -> None:
        schemas = list((THIS_DIR / "schemas").glob("*.json")) + [ROOT / "bench" / "native" / "scenario.schema.json"]
        self.assertEqual(4, len(schemas))
        for path in schemas:
            document = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual("https://json-schema.org/draft/2020-12/schema", document["$schema"])
            self.assertTrue(document["$id"].startswith("https://php.net/schemas/native/"))

    def test_all_synthetic_examples_pass_strict_validator(self) -> None:
        examples = list((THIS_DIR / "examples").glob("*.json")) + list((ROOT / "bench" / "native" / "examples").glob("*.json"))
        self.assertEqual(3, len(examples))
        for path in examples:
            document = json.loads(path.read_text(encoding="utf-8"))
            self.assertTrue(document["synthetic"])
            validate_document(document)


if __name__ == "__main__":
    unittest.main()
