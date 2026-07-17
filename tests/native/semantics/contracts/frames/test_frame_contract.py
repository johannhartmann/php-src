import copy
import importlib.util
import json
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[5]
VALIDATOR_PATH = REPO_ROOT / "scripts/native/semantics/validate-frame-contract.py"
EXAMPLES_PATH = REPO_ROOT / "docs/native-engine/semantics/frames/frame-state.examples.json"
SCHEMA_PATH = REPO_ROOT / "docs/native-engine/semantics/frames/frame-state.schema.json"

SPEC = importlib.util.spec_from_file_location("validate_frame_contract", VALIDATOR_PATH)
VALIDATOR = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(VALIDATOR)


class FrameContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.document = json.loads(EXAMPLES_PATH.read_text(encoding="utf-8"))
        cls.schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
        cls.examples = {item["example_id"]: item for item in cls.document["examples"]}

    def test_complete_contract_passes(self):
        self.assertEqual(VALIDATOR.check(SCHEMA_PATH, EXAMPLES_PATH), 0)

    def test_all_declared_valid_examples_pass(self):
        for example in self.document["examples"]:
            if example["expected_valid"]:
                with self.subTest(example=example["example_id"]):
                    self.assertEqual(VALIDATOR.validate_example(example, self.schema), set())

    def test_every_declared_negative_example_fails_as_documented(self):
        negatives = [item for item in self.document["examples"] if not item["expected_valid"]]
        self.assertGreaterEqual(len(negatives), 5)
        for example in negatives:
            with self.subTest(example=example["example_id"]):
                self.assertEqual(
                    VALIDATOR.validate_example(example, self.schema),
                    set(example["expected_errors"]),
                )

    def test_parent_cycle_is_rejected(self):
        self.assertIn("parent-cycle", VALIDATOR.validate_example(self.examples["invalid-parent-cycle"]))

    def test_duplicate_slot_is_rejected(self):
        self.assertIn("duplicate-slot", VALIDATOR.validate_example(self.examples["invalid-duplicate-slot"]))

    def test_missing_root_is_rejected(self):
        self.assertIn("missing-root", VALIDATOR.validate_example(self.examples["invalid-missing-root"]))

    def test_missing_cleanup_is_rejected(self):
        self.assertIn("missing-cleanup", VALIDATOR.validate_example(self.examples["invalid-missing-cleanup"]))

    def test_arbitrary_resume_target_and_version_mismatch_are_rejected(self):
        errors = VALIDATOR.validate_example(self.examples["invalid-resume-target"], self.schema)
        self.assertEqual(
            errors,
            {"invalid-resume-target", "resume-version-mismatch", "schema-validation"},
        )

    def test_missing_required_field_is_rejected(self):
        example = copy.deepcopy(self.examples["normal-call"])
        del example["frames"][0]["continuations"]
        self.assertIn("missing-required-field", VALIDATOR.validate_example(example))

    def test_unlisted_root_is_rejected(self):
        example = copy.deepcopy(self.examples["normal-call"])
        example["frames"][0]["roots"].append("normal-caller:ghost")
        self.assertIn("missing-root", VALIDATOR.validate_example(example))

    def test_resume_requires_suspend_state(self):
        example = copy.deepcopy(self.examples["generator-suspend"])
        example["frames"][0]["suspend"] = {"kind": "none", "state_id": None}
        self.assertIn("invalid-resume-target", VALIDATOR.validate_example(example))

    def test_internal_frame_uses_caller_opline(self):
        example = copy.deepcopy(self.examples["normal-call"])
        frame = example["frames"][1]
        frame["function"] = {"kind": "internal", "name": "strlen", "op_array_id": None}
        frame["opline"] = {"index": None, "phase": "before", "owner_frame_id": "normal-caller"}
        self.assertEqual(VALIDATOR.validate_example(example), set())

    def test_internal_frame_cannot_claim_user_opline(self):
        example = copy.deepcopy(self.examples["normal-call"])
        frame = example["frames"][1]
        frame["function"] = {"kind": "internal", "name": "strlen", "op_array_id": None}
        self.assertIn("invalid-opline-owner", VALIDATOR.validate_example(example))

    def test_schema_rejects_invalid_enum_and_additional_property(self):
        example = copy.deepcopy(self.examples["normal-call"])
        frame = example["frames"][0]
        frame["safepoint"]["class"] = "not-a-safepoint"
        frame["backend_register"] = "rax"
        self.assertIn("schema-validation", VALIDATOR.validate_example(example, self.schema))


if __name__ == "__main__":
    unittest.main()
