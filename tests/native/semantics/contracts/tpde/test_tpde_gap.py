"""Contract tests for the W01 TPDE capability inventory validator."""

from __future__ import annotations

import copy
import importlib.util
import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[5]
VALIDATOR_PATH = ROOT / "scripts/native/semantics/validate-tpde-gap.py"
MANIFEST_PATH = ROOT / "docs/native-engine/tpde/required-capabilities.json"


def load_validator():
    spec = importlib.util.spec_from_file_location("validate_tpde_gap", VALIDATOR_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load TPDE gap validator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class TpdeGapContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.validator = load_validator()
        cls.manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))

    def errors_for(self, data):
        return self.validator.validate_document(data)

    def assert_error_contains(self, data, fragment: str) -> None:
        errors = self.errors_for(data)
        self.assertTrue(
            any(fragment in error for error in errors),
            f"expected {fragment!r} in errors: {errors}",
        )

    def test_repository_manifest_is_structurally_valid(self) -> None:
        self.assertEqual([], self.errors_for(copy.deepcopy(self.manifest)))

    def test_missing_pin_is_rejected(self) -> None:
        data = copy.deepcopy(self.manifest)
        del data["tpde_commit"]
        self.assert_error_contains(data, "tpde_commit must be the pinned commit")

    def test_unresolved_critical_gap_is_rejected(self) -> None:
        data = copy.deepcopy(self.manifest)
        capability = data["capabilities"][0]
        capability["classification"] = "blocked"
        self.assert_error_contains(data, "critical capability remains unresolved")

    def test_source_ref_without_symbol_is_rejected(self) -> None:
        data = copy.deepcopy(self.manifest)
        data["capabilities"][0]["source_refs"][0]["symbol"] = ""
        self.assert_error_contains(data, ".symbol must be a non-empty string")

    def test_false_covered_claim_without_evidence_is_rejected(self) -> None:
        data = copy.deepcopy(self.manifest)
        capability = next(
            cap for cap in data["capabilities"] if cap["classification"] == "covered"
        )
        capability["source_refs"] = []
        self.assert_error_contains(data, ".source_refs must contain evidence")

    def test_unsupported_target_claim_is_rejected(self) -> None:
        data = copy.deepcopy(self.manifest)
        data["capabilities"][0]["target_impact"]["aarch64"]["status"] = "required"
        self.assert_error_contains(data, "AArch64 is analyzed but not released")


if __name__ == "__main__":
    unittest.main()
