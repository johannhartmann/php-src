"""Tests for non-circular committed W05 task results."""

from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]
WAVE_GATE_PATH = ROOT / "scripts/native/wave-gate.py"
DEFINITION_PATH = ROOT / "docs/native-engine/waves/waves.json"
FIXTURE_PATH = (
    ROOT / "docs/native-engine/waves/fixtures/valid/task-result-D.json"
)

SPEC = importlib.util.spec_from_file_location("sealed_result_wave_gate", WAVE_GATE_PATH)
assert SPEC is not None and SPEC.loader is not None
WAVE_GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(WAVE_GATE)


def sealed_result() -> dict:
    result = json.loads(FIXTURE_PATH.read_text(encoding="utf-8"))
    phases = (
        "W05-v2-contract",
        "W05-v2-wave-pin",
        "W05-v2-implementation",
        "W05-v2-gate",
    )
    result.update(
        {
            "task_id": "W05-integration-gate",
            "head_commit": None,
            "tested_head_commit": "a" * 40,
            "seal_subject": {
                "receipt_path": "docs/native-engine/waves/receipts/W05.json",
                "receipt_sha256": "b" * 64,
            },
            "phase_receipts": [
                {
                    "format_version": "1.0.0",
                    "phase_id": phase,
                    "commit": ("%x" % (index + 1)) * 40,
                    "tree": ("%x" % (index + 5)) * 40,
                    "parent": ("%x" % (index + 9)) * 40,
                    "changed_paths": ["path/file.c"],
                    "command_summary_digests": [],
                }
                for index, phase in enumerate(phases)
            ],
        }
    )
    for evidence in result["gate_evidence"]:
        evidence["wave_id"] = "W05"
        evidence["gate_id"] = "W05-integration-gate"
    return result


class SealedTaskResultTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.definition = WAVE_GATE.load_definition(DEFINITION_PATH)

    def test_non_circular_w05_result_is_valid(self) -> None:
        self.assertEqual(
            [],
            WAVE_GATE.validate_result(sealed_result(), self.definition),
        )

    def test_w05_result_requires_tested_head_and_receipt_binding(self) -> None:
        for missing in (
            "tested_head_commit", "seal_subject", "phase_receipts",
        ):
            with self.subTest(missing=missing):
                result = sealed_result()
                del result[missing]
                issues = WAVE_GATE.validate_result(result, self.definition)
                self.assertTrue(any(missing in issue for issue in issues), issues)

    def test_w05_result_rejects_self_referential_head(self) -> None:
        result = sealed_result()
        result["head_commit"] = "c" * 40
        issues = WAVE_GATE.validate_result(result, self.definition)
        self.assertTrue(any("must be null" in issue for issue in issues), issues)

    def test_gate_uses_tested_head_as_effective_head(self) -> None:
        result = sealed_result()
        wave = next(
            item for item in self.definition["waves"] if item["wave_id"] == "W05"
        )
        wave = copy.deepcopy(wave)
        wave["expected_base_commit"] = result["expected_base_commit"]
        task = wave["tasks"][0]
        self.assertNotIn(
            "has no tested head commit",
            WAVE_GATE.task_failures(result, wave, task),
        )
        self.assertEqual("a" * 40, WAVE_GATE.result_tested_head(result))


if __name__ == "__main__":
    unittest.main()
