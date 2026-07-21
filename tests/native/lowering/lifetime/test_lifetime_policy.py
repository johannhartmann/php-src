from __future__ import annotations

import json
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[4]
STRAIGHT_LINE = ROOT / "Zend/Native/Lowering/StraightLine"
PROFILE = ROOT / "docs/native-engine/lowering/w03-opcode-profile.json"


class LifetimePolicyTests(unittest.TestCase):
    def test_profile_claims_are_exact_and_disjoint(self) -> None:
        entries = json.loads(PROFILE.read_text(encoding="utf-8"))["opcodes"]
        owned = {
            entry["opcode"]: (entry["number"], entry["mir_opcodes"])
            for entry in entries
            if entry["provider"] == "lifetime"
        }
        self.assertEqual(
            {
                "ZEND_RETURN": (62, ["return"]),
                "ZEND_FREE": (70, ["scalar_drop"]),
            },
            owned,
        )
        provider = (STRAIGHT_LINE / "zend_mir_lifetime_provider.c").read_text()
        self.assertIn("ZEND_MIR_STRAIGHT_LINE_OPCODE_QM_ASSIGN,", provider)
        self.assertIn("ZEND_MIR_STRAIGHT_LINE_OPCODE_RETURN", provider)
        self.assertIn("ZEND_MIR_STRAIGHT_LINE_OPCODE_FREE", provider)
        self.assertNotIn("ZEND_MIR_STRAIGHT_LINE_OPCODE_NOP,", provider)
        self.assertRegex(
            provider,
            re.compile(
                r"ZEND_MIR_STRAIGHT_LINE_OPCODE_QM_ASSIGN\).*?"
                r"zend_mir_lower_copy_move",
                re.DOTALL,
            ),
        )

    def test_no_runtime_or_target_dependency(self) -> None:
        combined = "\n".join(
            path.read_text(encoding="utf-8")
            for path in sorted(STRAIGHT_LINE.glob("*.[ch]"))
        )
        forbidden = re.compile(
            r"#\s*include[^\n]*(?:zend_vm|zend_execute|tpde|x86|aarch64|arm64|riscv)"
            r"|\b(?:zend_execute_data|zend_vm_call_opcode_handler|execute_ex)\b",
            re.IGNORECASE,
        )
        self.assertIsNone(forbidden.search(combined))

    def test_free_emits_profiled_drop_without_destructor_semantics(self) -> None:
        source = (STRAIGHT_LINE / "zend_mir_lower_return.c").read_text()
        self.assertNotIn("ZEND_MIR_OWNERSHIP_ACTION_DESTROY", source)
        self.assertNotIn("ZEND_MIR_OWNERSHIP_ACTION_CONDITIONAL_DESTROY", source)
        self.assertIn("ZEND_MIR_OPCODE_SCALAR_DROP", source)


if __name__ == "__main__":
    unittest.main()
