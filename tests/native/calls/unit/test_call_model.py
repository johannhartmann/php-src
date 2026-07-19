from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
MODEL = ROOT / "Zend/Native/Calls/Model/zend_mir_call_model.c"
MODULE = ROOT / "Zend/Native/MIR/Core/zend_mir_module.c"
SOURCE = Path(__file__).with_name("test_call_contract.c")


class CallModelTests(unittest.TestCase):
    def test_public_contract_compiles_and_runs_under_c11(self) -> None:
        with tempfile.TemporaryDirectory(prefix="w05-call-unit-") as directory:
            executable = Path(directory) / "test_call_contract"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I.",
                    str(SOURCE),
                    "-o",
                    str(executable),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(executable)], cwd=ROOT, check=True)

    def test_planner_precedes_w04_and_all_mutation(self) -> None:
        text = MODEL.read_text(encoding="utf-8")
        planner = text.index("code = zend_mir_w05_plan_calls(")
        w04 = text.index("w04 = zend_mir_lower_w04_zend_source(")
        emitter = text.index("zend_mir_w05_emit_calls(&plan")
        self.assertLess(planner, w04)
        self.assertLess(w04, emitter)

    def test_plan_is_frozen_and_codegen_debt_stays_open(self) -> None:
        text = MODEL.read_text(encoding="utf-8")
        self.assertIn("plan->public_plan.complete = true;", text)
        self.assertIn("plan->public_plan.immutable = true;", text)
        self.assertIn("receipt.codegen_eligible = false;", text)
        self.assertIn("ZEND_MIR_W05_REQUIRED_DEBTS", text)

    def test_module_publication_uses_staging_and_atomic_commit(self) -> None:
        text = MODULE.read_text(encoding="utf-8")
        self.assertIn("zend_mir_core_call_staging", text)
        self.assertIn("zend_mir_core_commit_call_model", text)
        self.assertIn("staging->committed = true;", text)
        self.assertIn("free(module->call_staging.targets);", text)

    def test_model_contains_no_runtime_or_target_backend(self) -> None:
        text = MODEL.read_text(encoding="utf-8").lower()
        for token in (
            "zend_execute",
            "execute_ex",
            "zend_vm_call_opcode_handler",
            "mir_interpret",
            "tpde",
            "x86",
            "aarch64",
        ):
            self.assertNotIn(token, text)


if __name__ == "__main__":
    unittest.main()
