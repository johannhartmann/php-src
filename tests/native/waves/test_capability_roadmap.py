import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]
ROADMAP_PATH = ROOT / "docs/native-engine/roadmap/capabilities.json"
RECEIPT_SCHEMA_PATH = (
    ROOT / "docs/native-engine/waves/schemas/wave-receipt.schema.json"
)
LEDGER_SCHEMA_PATH = ROOT / "docs/native-engine/waves/ledger.schema.json"


class CapabilityRoadmapTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.roadmap = json.loads(ROADMAP_PATH.read_text(encoding="utf-8"))

    def test_capability_ids_are_unique_and_prerequisites_exist(self):
        capabilities = self.roadmap["capabilities"]
        by_id = {item["capability_id"]: item for item in capabilities}
        self.assertEqual(len(capabilities), len(by_id))
        for item in capabilities:
            for prerequisite in item["prerequisites"]:
                self.assertIn(prerequisite, by_id)

    def test_w05_is_modeling_only_and_c_abi_remains_w15(self):
        by_id = {
            item["capability_id"]: item
            for item in self.roadmap["capabilities"]
        }
        w05 = {
            item["capability_id"]
            for item in self.roadmap["capabilities"]
            if item["wave_id"] == "W05"
        }
        self.assertEqual(
            {
                "direct_user_call_sequence",
                "caller_frame_descriptor",
                "callee_entry_descriptor",
                "abstract_call_effects",
            },
            w05,
        )
        self.assertEqual("W15", by_id["internal_c_abi_interop"]["wave_id"])
        self.assertIn(
            "call_execution",
            by_id["direct_user_call_sequence"]["open_debts"],
        )

    def test_target_emission_requires_empty_debt_policy(self):
        target = next(
            item for item in self.roadmap["capabilities"]
            if item["capability_id"] == "target_emission"
        )
        self.assertIn("suspended_call_frames", target["prerequisites"])
        self.assertIn("cow_indirect_semantics", target["prerequisites"])
        self.assertEqual([], target["open_debts"])

    def test_schemas_are_strict_draft_2020_12_documents(self):
        for path in (RECEIPT_SCHEMA_PATH, LEDGER_SCHEMA_PATH):
            schema = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(
                "https://json-schema.org/draft/2020-12/schema",
                schema["$schema"],
            )
            self.assertFalse(schema["additionalProperties"])
            self.assertEqual("object", schema["type"])


if __name__ == "__main__":
    unittest.main()
