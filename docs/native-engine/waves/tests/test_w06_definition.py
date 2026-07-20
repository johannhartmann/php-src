from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest


sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[4]
DEFINITION_PATH = ROOT / "docs/native-engine/waves/waves.json"
MANIFEST_PATH = ROOT / "docs/native-engine/values/w06-phase-manifest.json"
WAVE_GATE_PATH = ROOT / "scripts/native/wave-gate.py"
H = "6483d2593c5ec877f63bd1625a1609ab762abb27"

SPEC = importlib.util.spec_from_file_location("w06_wave_gate", WAVE_GATE_PATH)
assert SPEC is not None and SPEC.loader is not None
WAVE_GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(WAVE_GATE)


class W06DefinitionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.definition = WAVE_GATE.load_definition(DEFINITION_PATH)
        cls.waves, cls.tasks = WAVE_GATE.definition_indexes(cls.definition)
        cls.wave = cls.waves["W06"]
        cls.manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))

    def test_w06_is_pinned_to_h_and_has_one_task(self) -> None:
        self.assertEqual(H, self.wave["expected_base_commit"])
        self.assertEqual(
            "Zval storage, references, refcount transfer, and separation protocol",
            self.wave["title"],
        )
        self.assertEqual([], self.wave["parallel_tracks"])
        self.assertEqual(["W06-integration-gate"], self.wave["required_gate_ids"])
        self.assertEqual([], self.wave["optional_gate_ids"])
        self.assertEqual(1, len(self.wave["tasks"]))
        self.assertEqual("W06-integration-gate", self.wave["tasks"][0]["task_id"])

    def test_task_paths_are_generated_from_serial_writing_phases(self) -> None:
        expected: list[str] = []
        for phase_id in ("W06-core-A1", "W06-lowering-A2", "W06-gate-I", "W06-seal-S"):
            phase = next(
                item for item in self.manifest["phases"]
                if item["phase_id"] == phase_id
            )
            for path in phase["paths"]:
                if path not in expected:
                    expected.append(path)
        self.assertEqual(expected, self.wave["responsible_paths"])
        self.assertEqual(expected, self.wave["tasks"][0]["owned_paths"])

    def test_contract_and_pin_paths_are_not_task_owned(self) -> None:
        frozen = {
            path
            for phase in self.manifest["phases"][:2]
            for path in phase["paths"]
        }
        self.assertTrue(frozen.isdisjoint(self.wave["tasks"][0]["owned_paths"]))

    def test_definition_and_empty_view_are_deterministic(self) -> None:
        self.assertEqual([], WAVE_GATE.validate_definition(self.definition))
        with tempfile.TemporaryDirectory(prefix="w06-empty-wave-") as temporary:
            root = Path(temporary)
            first = WAVE_GATE.render_dashboard(self.definition, root, False)
            second = WAVE_GATE.render_dashboard(self.definition, root, False)
        self.assertEqual(first.encode("utf-8"), second.encode("utf-8"))
        section = first[first.index("## W06"):first.index("## W07")]
        self.assertIn("W06-integration-gate", section)
        self.assertIn("`MISSING`", section)


if __name__ == "__main__":
    unittest.main()
