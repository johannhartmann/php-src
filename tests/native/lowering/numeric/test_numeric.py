from __future__ import annotations

from pathlib import Path
import json
import re
import unittest


ROOT = Path(__file__).resolve().parents[4]
NUMERIC = ROOT / "Zend/Native/Lowering/Scalar/Numeric"


class NumericLoweringContractTests(unittest.TestCase):
    def test_deliverables_are_present(self) -> None:
        expected = {
            "zend_mir_lower_numeric.c",
            "zend_mir_lower_numeric.h",
            "zend_mir_numeric_proofs.c",
            "zend_mir_numeric_provider.c",
        }
        self.assertEqual(expected, {path.name for path in NUMERIC.iterdir()})

    def test_no_runtime_escape_hatches_are_emitted(self) -> None:
        lowering = (NUMERIC / "zend_mir_lower_numeric.c").read_text()
        for forbidden in (
            "ZEND_MIR_OPCODE_STATEPOINT",
            "add_block",
            "add_edge",
            "CALL_INTERNAL",
            "CALL_PHP",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, lowering)

    def test_proof_reduced_effect_profile_is_explicit(self) -> None:
        lowering = (NUMERIC / "zend_mir_lower_numeric.c").read_text()
        for field in (
            ".effects = 0",
            ".reads = 0",
            ".writes = 0",
            ".barriers = 0",
            ".ownership_actions = 0",
        ):
            with self.subTest(field=field):
                self.assertIn(field, lowering)

    def test_provider_claims_exclude_deferred_operations(self) -> None:
        provider = (NUMERIC / "zend_mir_numeric_provider.c").read_text()
        self.assertNotIn("ZEND_MIR_NUMERIC_OPCODE_DIV,", provider)
        self.assertNotIn("ZEND_MIR_NUMERIC_OPCODE_POW,", provider)
        self.assertNotIn("ZEND_MIR_NUMERIC_OPCODE_CONCAT,", provider)

    def test_profile_entries_have_exactly_one_claim(self) -> None:
        profile = json.loads(
            (
                ROOT
                / "docs/native-engine/lowering/w03-opcode-profile.json"
            ).read_text()
        )
        expected = {
            entry["number"]
            for entry in profile["opcodes"]
            if entry["provider"] == "numeric"
        }
        header = (NUMERIC / "zend_mir_lower_numeric.h").read_text()
        constants = {
            name: int(value)
            for name, value in re.findall(
                r"#define (ZEND_MIR_NUMERIC_OPCODE_[A-Z_]+) "
                r"UINT32_C\((\d+)\)",
                header,
            )
        }
        provider = (NUMERIC / "zend_mir_numeric_provider.c").read_text()
        claim_block = provider.split(
            "static bool zend_mir_numeric_group_claims", 1
        )[0]
        claimed_names = re.findall(
            r"\bZEND_MIR_NUMERIC_OPCODE_[A-Z_]+\b", claim_block
        )
        claimed = [constants[name] for name in claimed_names]
        self.assertEqual(expected, set(claimed))
        self.assertEqual(len(expected), len(claimed))


if __name__ == "__main__":
    unittest.main()
