from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import unittest


sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[4]
GENERATOR_PATH = ROOT / "scripts/native/calls/generate-w05-profile.py"
CONTRACT_PATH = ROOT / "scripts/native/calls/check-contract.py"


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


GENERATOR = load_module("w05_test_generator", GENERATOR_PATH)
CONTRACT = load_module("w05_test_contract", CONTRACT_PATH)


class OpcodeProfileTests(unittest.TestCase):
    def test_profile_is_exactly_live_derived(self) -> None:
        expected, expected_reclassification = GENERATOR.build()
        actual = json.loads(GENERATOR.W05_PROFILE.read_text(encoding="utf-8"))
        reclassified = json.loads(
            GENERATOR.RECLASSIFICATION.read_text(encoding="utf-8")
        )
        self.assertEqual(expected, actual)
        self.assertEqual(expected_reclassification, reclassified)
        self.assertEqual(0, reclassified["unresolved_w05_count"])
        self.assertEqual(len(actual["opcodes"]), actual["active_opcode_count"])

    def test_all_call_adjacent_opcodes_are_individually_decided(self) -> None:
        profile, _ = GENERATOR.build()
        adjacent = {
            entry["opcode"]
            for entry in profile["opcodes"]
            if GENERATOR.is_call_adjacent(entry)
        }
        self.assertEqual(set(GENERATOR.ACCEPTED) | set(GENERATOR.LATER), adjacent)
        self.assertFalse(
            any(entry.get("deferred_wave") == "W05" for entry in profile["opcodes"])
        )

    def test_accepted_fragments_require_atomic_sequence_proofs(self) -> None:
        profile, _ = GENERATOR.build()
        current = {entry["opcode"]: entry for entry in profile["opcodes"]}
        for opcode in GENERATOR.ACCEPTED:
            self.assertEqual("conditional", current[opcode]["classification"])
            self.assertEqual(GENERATOR.CALL_PROOFS, current[opcode]["proofs"])
            self.assertIn("never published independently", current[opcode]["rationale"])

    def test_generator_has_no_snapshot_opcode_count(self) -> None:
        source = GENERATOR_PATH.read_text(encoding="utf-8")
        self.assertNotIn("active_opcode_count\": 210", source)
        self.assertNotIn("active_opcode_count\": 212", source)


class SequenceContractTests(unittest.TestCase):
    def test_sequence_capabilities_debts_and_rejections(self) -> None:
        CONTRACT.validate_sequence_profile()
        CONTRACT.validate_fixtures()

    def test_headers_preserve_prior_contracts(self) -> None:
        CONTRACT.validate_headers()

    def test_call_site_carries_exact_scalar_result_identity(self) -> None:
        header = (ROOT / "Zend/Native/MIR/zend_mir_call.h").read_text(
            encoding="utf-8"
        )
        call_site = CONTRACT.extract_struct(header, "zend_mir_call_site_ref")
        self.assertIn("zend_mir_value_id result_id;", call_site)
        self.assertLess(call_site.find("arguments"), call_site.find("result_id"))

if __name__ == "__main__":
    unittest.main()
