from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest


sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[4]
GENERATOR_PATH = ROOT / "scripts/native/calls/generate-w05-profile.py"
PHASES_PATH = ROOT / "scripts/native/calls/check-phases.py"
CONTRACT_PATH = ROOT / "scripts/native/calls/check-contract.py"


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


GENERATOR = load_module("w05_test_generator", GENERATOR_PATH)
PHASES = load_module("w05_test_phases", PHASES_PATH)
CONTRACT = load_module("w05_test_contract", CONTRACT_PATH)


class OpcodeProfileTests(unittest.TestCase):
    def test_profile_is_exactly_live_derived(self) -> None:
        expected, changes = GENERATOR.build()
        actual = json.loads(GENERATOR.W05_PROFILE.read_text(encoding="utf-8"))
        reclassified = json.loads(
            GENERATOR.RECLASSIFICATION.read_text(encoding="utf-8")
        )
        self.assertEqual(expected, actual)
        self.assertEqual(changes, reclassified)
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


class PhaseManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.manifest = json.loads(PHASES.MANIFEST.read_text(encoding="utf-8"))

    def test_manifest_is_serial_and_disjoint(self) -> None:
        self.assertEqual([], PHASES.validate_manifest(self.manifest))
        actual = {
            phase["phase_id"]: tuple(phase["task_ids"])
            for phase in self.manifest["phases"]
        }
        self.assertEqual(PHASES.EXPECTED_TASKS, actual)

    def test_unplanned_overlap_is_rejected(self) -> None:
        mutated = copy.deepcopy(self.manifest)
        implementation = next(
            phase for phase in mutated["phases"]
            if phase["phase_id"] == "implementation"
        )
        implementation["paths"].append("docs/native-engine/calls/contracts/**")
        errors = PHASES.validate_manifest(mutated)
        self.assertTrue(any("phase path overlap" in error for error in errors))

    def test_command_use_before_provider_is_rejected(self) -> None:
        mutated = copy.deepcopy(self.manifest)
        contract = next(
            phase for phase in mutated["phases"]
            if phase["phase_id"] == "contract-H"
        )
        contract["commands_used"].append("scripts/native/calls/test-w05.py")
        errors = PHASES.validate_manifest(mutated)
        self.assertTrue(any("used before provider" in error for error in errors))

    def test_wrong_phase_task_mapping_is_rejected(self) -> None:
        mutated = copy.deepcopy(self.manifest)
        implementation = next(
            phase for phase in mutated["phases"]
            if phase["phase_id"] == "implementation"
        )
        implementation["task_ids"] = ["W05-A"]
        errors = PHASES.validate_manifest(mutated)
        self.assertTrue(any("task IDs" in error for error in errors))

    def test_only_contract_h_can_change_root_contracts(self) -> None:
        mutated = copy.deepcopy(self.manifest)
        gate = next(
            phase for phase in mutated["phases"] if phase["phase_id"] == "gate"
        )
        gate["root_contracts_mutable"] = True
        errors = PHASES.validate_manifest(mutated)
        self.assertTrue(any("root contracts" in error for error in errors))


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

    def test_w02_regression_uses_commit_bound_historical_report(self) -> None:
        source = CONTRACT_PATH.read_text(encoding="utf-8")
        self.assertIn("validate_w02_regressions()", source)
        self.assertIn(
            'run(["python3", "scripts/native/mir/validate-w02.py", "--check"])',
            source,
        )


if __name__ == "__main__":
    unittest.main()
