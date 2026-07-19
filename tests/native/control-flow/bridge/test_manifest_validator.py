"""Mutation tests for the W04 corpus manifest validator."""

from __future__ import annotations

import copy
import importlib.util
import unittest
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[4]
VALIDATOR_PATH = ROOT / "tests/native/control-flow/corpus/validate_manifest.py"


def load_validator() -> Any:
    specification = importlib.util.spec_from_file_location(
        "w04_manifest_validator_test", VALIDATOR_PATH
    )
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


validator = load_validator()


class ManifestValidatorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.document = validator.load_json(validator.MANIFEST)
        cls.schema = validator.load_json(validator.SCHEMA)

    def test_checked_in_manifest_is_valid(self) -> None:
        validator.validate_document(self.document, self.schema)

    def test_missing_required_intention_is_rejected(self) -> None:
        document = copy.deepcopy(self.document)
        document["cases"] = document["cases"][1:]
        with self.assertRaisesRegex(validator.ManifestError, "too few|matrix"):
            validator.validate_document(document, self.schema)

    def test_final_mir_hash_is_rejected(self) -> None:
        document = copy.deepcopy(self.document)
        case_schema = self.schema["$defs"]["case"]
        case_schema = copy.deepcopy(case_schema)
        case_schema["properties"]["mir_sha256"] = {"type": "string"}
        document["cases"][0]["mir_sha256"] = "0" * 64
        modified_schema = copy.deepcopy(self.schema)
        modified_schema["$defs"]["case"] = case_schema
        with self.assertRaisesRegex(validator.ManifestError, "final MIR field"):
            validator.validate_document(document, modified_schema)

    def test_accepted_call_boundary_is_rejected(self) -> None:
        case = copy.deepcopy(self.document["cases"][0])
        original = validator.source_function
        try:
            validator.source_function = lambda source, function: (
                "function {}(int $value)".format(function),
                "return strlen('x');",
            )
            with self.assertRaisesRegex(
                validator.ManifestError, "runtime/reference boundary"
            ):
                validator.validate_case(case)
        finally:
            validator.source_function = original

    def test_rejected_wave_and_diagnostic_are_stable(self) -> None:
        document = copy.deepcopy(self.document)
        case = next(
            item for item in document["cases"] if item["case_id"] == "call_inside_branch"
        )
        case["deferred_wave"] = "W06"
        with self.assertRaisesRegex(validator.ManifestError, "classification"):
            validator.validate_document(document, self.schema)

    def test_calls_one_through_ten_are_required(self) -> None:
        document = copy.deepcopy(self.document)
        document["cases"][0]["repeat_calls"] = list(range(1, 10))
        with self.assertRaisesRegex(validator.ManifestError, "too few|calls"):
            validator.validate_document(document, self.schema)


if __name__ == "__main__":
    unittest.main()
