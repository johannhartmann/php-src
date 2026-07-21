from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[4]
GENERATOR_PATH = ROOT / "scripts/native/control-flow/generate-w04-profile.py"
CONTRACT_PATH = ROOT / "scripts/native/control-flow/check-contract.py"


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


GENERATOR = load_module("w04_test_generator", GENERATOR_PATH)
CONTRACT = load_module("w04_test_contract", CONTRACT_PATH)


class OpcodeProfileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.matrix = json.loads(GENERATOR.W01_MATRIX.read_text(encoding="utf-8"))
        cls.w03 = json.loads(GENERATOR.W03_PROFILE.read_text(encoding="utf-8"))
        cls.profile = json.loads(GENERATOR.W04_PROFILE.read_text(encoding="utf-8"))
        cls.arguments = {
            "matrix_sha256": hashlib.sha256(GENERATOR.W01_MATRIX.read_bytes()).hexdigest(),
            "w03_sha256": hashlib.sha256(GENERATOR.W03_PROFILE.read_bytes()).hexdigest(),
        }

    def test_profile_is_live_derived_and_complete(self) -> None:
        errors = GENERATOR.validate_profile(
            self.profile, self.matrix, self.w03, **self.arguments
        )
        self.assertEqual([], errors)
        self.assertEqual(len(self.matrix["opcodes"]), self.profile["active_opcode_count"])

    def test_all_live_w04_deferrals_are_resolved(self) -> None:
        old = {
            entry["opcode"]
            for entry in self.w03["opcodes"]
            if entry.get("deferred_wave") == "W04"
        }
        current = {entry["opcode"]: entry for entry in self.profile["opcodes"]}
        self.assertTrue(old)
        for name in old:
            self.assertNotEqual("W04", current[name].get("deferred_wave"))

    def test_required_branch_subset_is_accepted(self) -> None:
        current = {entry["opcode"]: entry for entry in self.profile["opcodes"]}
        for name in (
            "ZEND_JMP",
            "ZEND_JMPZ",
            "ZEND_JMPNZ",
            "ZEND_JMPZ_EX",
            "ZEND_JMPNZ_EX",
        ):
            self.assertNotEqual("deferred", current[name]["classification"])

    def test_hash_drift_is_rejected(self) -> None:
        mutated = copy.deepcopy(self.profile)
        mutated["sources"]["w03"]["sha256"] = "0" * 64
        errors = GENERATOR.validate_profile(
            mutated, self.matrix, self.w03, **self.arguments
        )
        self.assertIn("profile differs from live W01/W03-derived output", errors)

    def test_classification_drift_is_rejected(self) -> None:
        mutated = copy.deepcopy(self.profile)
        jump = next(entry for entry in mutated["opcodes"] if entry["opcode"] == "ZEND_JMP")
        jump["classification"] = "deferred"
        jump["deferred_wave"] = "W04"
        errors = GENERATOR.validate_profile(
            mutated, self.matrix, self.w03, **self.arguments
        )
        self.assertTrue(any("unresolved W04 deferral" in error for error in errors))
        self.assertTrue(any("mandatory W04 acceptance" in error for error in errors))

    def test_generator_has_no_snapshot_opcode_count(self) -> None:
        source = GENERATOR_PATH.read_text(encoding="utf-8")
        self.assertNotIn("active_opcode_count\": 210", source)
        self.assertNotIn("active_opcode_count\": 212", source)


class SourceFixtureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        document = json.loads(
            (ROOT / "tests/native/control-flow/contracts/source-cfg-fixtures.json")
            .read_text(encoding="utf-8")
        )
        cls.cases = {case["name"]: case for case in document["cases"]}

    def test_diamond_loop_phi_and_pi_are_valid(self) -> None:
        for name in ("diamond", "loop", "pi-type-and-range"):
            with self.subTest(name=name):
                self.assertEqual([], CONTRACT.validate_source_fixture(self.cases[name]))

    def test_malformed_phi_order_is_rejected(self) -> None:
        errors = CONTRACT.validate_source_fixture(
            self.cases["malformed-phi-predecessor-order"]
        )
        self.assertIn("phi-predecessor-order", errors)


if __name__ == "__main__":
    unittest.main()
