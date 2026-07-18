from __future__ import annotations

import json
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[4]
PROFILE = ROOT / "docs/native-engine/lowering/w03-opcode-profile.json"
LOGIC = ROOT / "Zend/Native/Lowering/Scalar/Logic"


class LogicContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.profile = json.loads(PROFILE.read_text(encoding="utf-8"))
        cls.sources = {
            path.name: path.read_text(encoding="utf-8")
            for path in sorted(LOGIC.glob("*"))
            if path.suffix in {".c", ".h"}
        }
        cls.combined = "\n".join(cls.sources.values())

    def test_provider_claims_exactly_profile_owned_opcodes(self) -> None:
        owned = [
            entry["number"]
            for entry in self.profile["opcodes"]
            if entry["owner"] == "W03-D-comparison-boolean-casts"
        ]
        provider = self.sources["zend_mir_logic_provider.c"]
        claim_block = provider.split(
            "zend_mir_logic_claimed_opcodes[] = {", 1
        )[1].split("};", 1)[0]
        symbols = re.findall(r"ZEND_MIR_LOGIC_ZEND_[A-Z_]+", claim_block)
        header = self.sources["zend_mir_logic.h"]
        values = {
            symbol: int(value)
            for symbol, value in re.findall(
                r"(ZEND_MIR_LOGIC_ZEND_[A-Z_]+)\s*=\s*([0-9]+)", header
            )
        }
        self.assertEqual(owned, [values[symbol] for symbol in symbols])
        self.assertNotIn(123, owned)

    def test_every_profile_mir_opcode_is_represented(self) -> None:
        owned = [
            entry
            for entry in self.profile["opcodes"]
            if entry["owner"] == "W03-D-comparison-boolean-casts"
        ]
        expected = {
            opcode.upper()
            for entry in owned
            for opcode in entry["mir_opcodes"]
        }
        for opcode in expected:
            self.assertIn(f"ZEND_MIR_OPCODE_{opcode}", self.combined)

    def test_no_runtime_helper_locale_or_fast_math_dependency(self) -> None:
        forbidden = (
            r"\b(?:zval|zend_op|zend_execute|zend_vm|zend_jit|setlocale|localeconv"
            r"|strtod|strtol|isnan|isinf|fast-math|TPDE|DynASM)\b"
        )
        self.assertIsNone(re.search(forbidden, self.combined, re.IGNORECASE))
        for text in self.sources.values():
            includes = re.findall(r'#\s*include\s*[<"]([^">]+)', text)
            self.assertFalse(
                any(
                    name in include.lower()
                    for include in includes
                    for name in ("zend_vm", "zend_execute", "zend_operators", "math.h")
                )
            )

    def test_mutation_occurs_only_in_provider_commit(self) -> None:
        for name, text in self.sources.items():
            if name == "zend_mir_logic_provider.c":
                continue
            self.assertNotIn("add_value(", text)
            self.assertNotIn("add_instruction(", text)
            self.assertNotIn("add_operand(", text)
            self.assertNotIn("add_value_fact(", text)
        provider = self.sources["zend_mir_logic_provider.c"]
        self.assertIn("zend_mir_logic_commit_plan", provider)
        self.assertIn("status != ZEND_MIR_LOWERING_SUCCESS", provider)

    def test_all_emitted_results_are_effect_free_and_owned(self) -> None:
        provider = self.sources["zend_mir_logic_provider.c"]
        self.assertIn("memset(&instruction, 0, sizeof(instruction))", provider)
        self.assertNotIn("ZEND_MIR_OWNERSHIP_ACTION_PRODUCE_OWNED", provider)
        self.assertNotRegex(provider, r"instruction\.(?:effects|reads|writes|barriers)\s*=")


if __name__ == "__main__":
    unittest.main()
