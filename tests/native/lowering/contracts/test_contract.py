from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[4]
GENERATOR = ROOT / "scripts/native/lowering/generate-profile.py"
MATRIX = ROOT / "docs/native-engine/semantics/opcodes/opcode-matrix.json"
PROFILE = ROOT / "docs/native-engine/lowering/w03-opcode-profile.json"


def load_generator():
    spec = importlib.util.spec_from_file_location("w03_profile_tests", GENERATOR)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class LoweringContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.generator = load_generator()
        cls.matrix = json.loads(MATRIX.read_text())
        cls.profile = json.loads(PROFILE.read_text())

    def test_contract_checker(self) -> None:
        subprocess.run(
            ["python3", "scripts/native/lowering/check-contract.py", "--check"],
            cwd=ROOT,
            check=True,
        )

    def test_profile_is_complete(self) -> None:
        self.assertEqual([], self.generator.validate_profile(self.profile, self.matrix))
        self.assertEqual(212, len(self.profile["opcodes"]))

    def test_missing_conditional_proof_is_rejected(self) -> None:
        mutated = copy.deepcopy(self.profile)
        entry = next(item for item in mutated["opcodes"] if item["classification"] == "conditional")
        entry["proofs"] = []
        errors = self.generator.validate_profile(mutated, self.matrix)
        self.assertTrue(any("no proofs" in error for error in errors))

    def test_unknown_proof_is_rejected(self) -> None:
        mutated = copy.deepcopy(self.profile)
        entry = next(item for item in mutated["opcodes"] if item["classification"] == "conditional")
        entry["proofs"].append("permissive_guess")
        errors = self.generator.validate_profile(mutated, self.matrix)
        self.assertTrue(any("unknown proofs" in error for error in errors))

    def test_duplicate_opcode_is_rejected(self) -> None:
        mutated = copy.deepcopy(self.profile)
        mutated["opcodes"][1]["number"] = mutated["opcodes"][0]["number"]
        errors = self.generator.validate_profile(mutated, self.matrix)
        self.assertTrue(any("duplicate" in error for error in errors))

    def test_mandatory_exclusions_are_deferred(self) -> None:
        decisions = {entry["opcode"]: entry for entry in self.profile["opcodes"]}
        for opcode in ("ZEND_DIV", "ZEND_POW", "ZEND_ASSIGN", "ZEND_JMP", "ZEND_DO_FCALL"):
            self.assertEqual("deferred", decisions[opcode]["classification"])


if __name__ == "__main__":
    unittest.main()
