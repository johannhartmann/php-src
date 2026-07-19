from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[4]
GENERATOR_PATH = ROOT / "scripts/native/control-flow/generate-w04-profile.py"
OWNERSHIP_PATH = ROOT / "scripts/native/control-flow/check-ownership.py"
CONTRACT_PATH = ROOT / "scripts/native/control-flow/check-contract.py"
W02_VALIDATOR_PATH = ROOT / "scripts/native/mir/validate-w02.py"
W03_VALIDATOR_PATH = ROOT / "scripts/native/lowering/validate-w03.py"


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


GENERATOR = load_module("w04_test_generator", GENERATOR_PATH)
OWNERSHIP = load_module("w04_test_ownership", OWNERSHIP_PATH)
CONTRACT = load_module("w04_test_contract", CONTRACT_PATH)
W02_VALIDATOR = load_module("w04_test_w02_validator", W02_VALIDATOR_PATH)
W03_VALIDATOR = load_module("w04_test_w03_validator", W03_VALIDATOR_PATH)


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


class OwnershipTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.manifest = json.loads(OWNERSHIP.MANIFEST.read_text(encoding="utf-8"))
        cls.sources = json.loads(OWNERSHIP.SOURCE_MANIFEST.read_text(encoding="utf-8"))

    def test_manifest_is_disjoint(self) -> None:
        self.assertEqual([], OWNERSHIP.validate_manifest(self.manifest, self.sources))

    def test_specialist_overlap_is_rejected(self) -> None:
        mutated = copy.deepcopy(self.manifest)
        mutated["specialist_tasks"][1]["owned_paths"].append(
            "Zend/Native/Lowering/ControlFlow/**"
        )
        errors = OWNERSHIP.validate_manifest(mutated, self.sources)
        self.assertTrue(any("specialist overlap" in error for error in errors))

    def test_reserved_integration_path_is_rejected(self) -> None:
        mutated = copy.deepcopy(self.manifest)
        mutated["specialist_tasks"][0]["owned_paths"].append(
            "ext/native_mir_test/config.m4"
        )
        errors = OWNERSHIP.validate_manifest(mutated, self.sources)
        self.assertTrue(any("reserved/integration path" in error for error in errors))

    def test_wave_definition_is_generated_from_manifest(self) -> None:
        base = "1" * 40
        wave = OWNERSHIP.build_wave(self.manifest, base)
        self.assertEqual("W04", wave["wave_id"])
        self.assertEqual("Control flow and loops", wave["title"])
        self.assertEqual(base, wave["expected_base_commit"])
        self.assertEqual(2, len(wave["parallel_tracks"]))
        self.assertEqual(
            [
                "W04-A-production-control-flow",
                "W04-B-control-flow-evidence",
                "W04-integration-gate",
            ],
            wave["required_gate_ids"],
        )
        for generated, source in zip(wave["tasks"], [
            *self.manifest["specialist_tasks"],
            self.manifest["integration_task"],
        ]):
            self.assertEqual(source["owned_paths"], generated["owned_paths"])

    def test_prerequisite_repairs_are_reserved(self) -> None:
        reserved = set(self.manifest["contract_reserved_paths"])
        for path in (
            "Zend/Native/MIR/Core/zend_mir_module.c",
            "Zend/Native/MIR/Core/zend_mir_view.c",
            "scripts/native/lowering/test-w03.py",
            "docs/native-engine/mir/w02-coverage-report.json",
            "docs/native-engine/lowering/w03-coverage-report.json",
        ):
            with self.subTest(path=path):
                self.assertIn(path, reserved)

    def test_wave_writer_appends_then_replaces_w04(self) -> None:
        original = OWNERSHIP.WAVES
        try:
            with tempfile.TemporaryDirectory(prefix="w04-wave-writer-") as directory:
                path = Path(directory) / "waves.json"
                path.write_text(
                    json.dumps({"waves": [{"wave_id": "W03"}]}) + "\n",
                    encoding="utf-8",
                )
                OWNERSHIP.WAVES = path
                OWNERSHIP.write_wave_definition(self.manifest, "1" * 40)
                first = json.loads(path.read_text(encoding="utf-8"))
                self.assertEqual(
                    ["W03", "W04"],
                    [wave["wave_id"] for wave in first["waves"]],
                )
                OWNERSHIP.write_wave_definition(self.manifest, "2" * 40)
                second = json.loads(path.read_text(encoding="utf-8"))
                w04 = [
                    wave for wave in second["waves"]
                    if wave["wave_id"] == "W04"
                ]
                self.assertEqual(1, len(w04))
                self.assertEqual("2" * 40, w04[0]["expected_base_commit"])
        finally:
            OWNERSHIP.WAVES = original


class PriorWaveReportTests(unittest.TestCase):
    def test_w02_inventory_excludes_w04_control_flow_sources(self) -> None:
        paths = (
            *W02_VALIDATOR.production_sources(),
            *W02_VALIDATOR.production_headers(),
        )
        self.assertTrue(paths)
        self.assertFalse(
            any(
                "ControlFlow"
                in path.relative_to(W02_VALIDATOR.MIR).parts
                for path in paths
            )
        )

    def test_w03_inventory_is_stable_across_w04_additions(self) -> None:
        paths = W03_VALIDATOR.production_paths()
        self.assertTrue(paths)
        self.assertFalse(
            any(
                "ControlFlow"
                in path.relative_to(W03_VALIDATOR.LOWERING).parts
                for path in paths
                if path.is_relative_to(W03_VALIDATOR.LOWERING)
            )
        )
        self.assertFalse(
            any(path.name == "zend_mir_lowering_w04.h" for path in paths)
        )
        source = W03_VALIDATOR_PATH.read_text(encoding="utf-8")
        function = source[
            source.index("def path_set_digest"):
            source.index("def contract_evidence_digest")
        ]
        self.assertNotIn("read_bytes", function)


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
