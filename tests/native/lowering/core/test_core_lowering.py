from __future__ import annotations

import json
import os
from pathlib import Path
import re
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[4]
CORE = ROOT / "Zend" / "Native" / "Lowering" / "Core"
RUNNER = Path(__file__).with_name("run_core_lowering_tests.py")


class CoreLoweringTests(unittest.TestCase):
    def test_strict_c_and_cxx_harness(self) -> None:
        subprocess.run(
            [sys.executable, str(RUNNER), "--cc", os.environ.get("CC", "cc")],
            cwd=ROOT,
            check=True,
            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
        )

    def test_w03_profile_assigns_nop_to_core_without_mir_semantics(self) -> None:
        profile = json.loads(
            (ROOT / "docs/native-engine/lowering/w03-opcode-profile.json").read_text()
        )
        nop = next(entry for entry in profile["opcodes"] if entry["opcode"] == "ZEND_NOP")
        self.assertEqual(nop["number"], 0)
        self.assertEqual(nop["provider"], "core")
        self.assertEqual(nop["mir_opcodes"], [])

    def test_compiled_profile_matches_every_frozen_w03_entry(self) -> None:
        profile = json.loads(
            (ROOT / "docs/native-engine/lowering/w03-opcode-profile.json").read_text()
        )
        registry_source = (CORE / "zend_mir_lowering_registry.c").read_text()
        compiled = {
            int(number): disposition
            for number, disposition in re.findall(
                r"\{UINT32_C\((\d+)\), "
                r"(ZEND_MIR_LOWERING_PROFILE_[A-Z0-9_]+)\}",
                registry_source,
            )
        }
        expected = {}
        for entry in profile["opcodes"]:
            if entry["classification"] in {"required", "conditional"}:
                disposition = "ZEND_MIR_LOWERING_PROFILE_ACCEPTED"
            elif entry["deferred_wave"] in {"W04", "W05", "W06"}:
                disposition = (
                    "ZEND_MIR_LOWERING_PROFILE_DEFERRED_"
                    + entry["deferred_wave"]
                )
            else:
                disposition = "ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER"
            expected[entry["number"]] = disposition
        self.assertEqual(compiled, expected)

    def test_orchestrator_contains_no_scalar_or_vm_fallback_semantics(self) -> None:
        orchestrator = (CORE / "zend_mir_lowering.c").read_text()
        forbidden = (
            "ZEND_MIR_OPCODE_I64_",
            "ZEND_MIR_OPCODE_F64_",
            "execute_ex",
            "zend_execute",
            "zend_vm_",
        )
        for token in forbidden:
            with self.subTest(token=token):
                self.assertNotIn(token, orchestrator)

    def test_verification_order_is_fail_closed(self) -> None:
        orchestrator = (CORE / "zend_mir_lowering.c").read_text()
        seal = orchestrator.index("mutator->seal_function")
        finalize = orchestrator.index("module_ops.finalize", seal)
        stage1 = orchestrator.index("module_ops.verify_stage1", finalize)
        stage2 = orchestrator.index("module_ops.verify_stage2", stage1)
        success = orchestrator.index(
            "ZEND_MIR_LOWERING_GUARANTEE_ALL", stage2
        )
        self.assertLess(seal, finalize)
        self.assertLess(finalize, stage1)
        self.assertLess(stage1, stage2)
        self.assertLess(stage2, success)


if __name__ == "__main__":
    unittest.main()
