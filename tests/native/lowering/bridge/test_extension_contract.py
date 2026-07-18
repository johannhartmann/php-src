"""Static contract checks for the default-off W03 test extension."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
EXTENSION = ROOT / "ext/native_mir_test/native_mir_test.c"
CONFIG = ROOT / "ext/native_mir_test/config.m4"
INVOKER = ROOT / "tests/native/lowering/bridge/invoke_dump.php"


class ExtensionContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.extension = EXTENSION.read_text(encoding="utf-8")
        cls.config = CONFIG.read_text(encoding="utf-8")
        cls.invoker = INVOKER.read_text(encoding="utf-8")

    def test_extension_is_static_and_disabled_by_default(self) -> None:
        self.assertRegex(
            self.config,
            re.compile(
                r"PHP_ARG_ENABLE\(\[native-mir-test\].*?\[no\],\s*\[no\]\)",
                re.DOTALL,
            ),
        )
        self.assertIn("cannot be built shared", self.config)
        self.assertRegex(
            self.config,
            re.compile(
                r"PHP_NEW_EXTENSION\(\[native_mir_test\],\s*"
                r"\[native_mir_test\.c\],\s*\[no\]",
                re.DOTALL,
            ),
        )

    def test_compile_dump_path_has_cleanup_and_bailout_boundaries(self) -> None:
        for required in (
            "zend_compile_string(",
            "zend_optimize_script(",
            "zend_dfa_analyze_op_array(",
            "zend_mir_lower_w03_zend_source(",
            "zend_mir_verify_stage1(",
            "zend_mir_verify_w03_scalar(",
            "zend_mir_dump_text(",
            "ZEND_COMPILE_WITHOUT_EXECUTION",
            "zend_try {",
            "} zend_catch {",
            "native_mir_test_cleanup(state);",
        ):
            self.assertIn(required, self.extension)

    def test_required_arguments_are_projected_to_entry_live_ins(self) -> None:
        integration = (
            ROOT / "Zend/Native/Lowering/Core/zend_mir_lowering_providers.c"
        ).read_text(encoding="utf-8")
        for required in (
            "op_array->opcodes[index].opcode == ZEND_RECV",
            "opline->opcode != ZEND_RECV",
            "opline->opcode = ZEND_NOP",
            "zend_mir_w03_clear_ssa_operand(",
            "integration->projected_literals",
            "RT_CONSTANT(original_opline, original_opline->op1)",
            "RT_CONSTANT(original_opline, original_opline->op2)",
        ):
            self.assertIn(required, integration)

    def test_compiled_function_table_entries_are_removed_during_cleanup(self) -> None:
        self.assertIn("state->function_table_used_before", self.extension)
        self.assertRegex(
            self.extension,
            re.compile(
                r"while \(index > state->function_table_used_before\).*?"
                r"zend_hash_del\(function_table, bucket->key\)",
                re.DOTALL,
            ),
        )

    def test_dump_path_has_no_execution_or_mir_evaluator(self) -> None:
        self.assertIsNone(
            re.search(
                r"\b(?:eval|include|include_once|require|require_once)\s*[\s(]",
                self.invoker,
                re.IGNORECASE,
            )
        )
        forbidden_native = re.compile(
            r"\b(?:zend_execute|execute_ex|zend_vm_call_opcode_handler|"
            r"mir_interpret|mir_evaluate)\b",
            re.IGNORECASE,
        )
        self.assertIsNone(forbidden_native.search(self.extension))

    def test_bridge_uses_structured_values_not_pointer_output(self) -> None:
        self.assertNotRegex(self.extension, r"%[pP]")
        self.assertNotIn("ZEND_REGISTER_RESOURCE", self.extension)
        for field in (
            '"diagnostics"',
            '"mir"',
            '"phase"',
            '"schema_version"',
            '"source"',
            '"source_id"',
            '"status"',
        ):
            self.assertIn(field, self.extension)

    def test_dump_failure_fault_rejects_the_first_write(self) -> None:
        self.assertRegex(
            self.extension,
            re.compile(
                r"if \(state->fault == NATIVE_MIR_TEST_FAULT_DUMP_FAILURE\)"
                r"\s*\{\s*state->dump_writes\+\+;\s*return false;",
                re.DOTALL,
            ),
        )


if __name__ == "__main__":
    unittest.main()
