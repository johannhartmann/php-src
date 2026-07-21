"""Static checks for the additive W04 test-extension bridge."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
EXTENSION = ROOT / "ext/native_mir_test/native_mir_test.c"
STUB = ROOT / "ext/native_mir_test/native_mir_test.stub.php"
CONFIG = ROOT / "ext/native_mir_test/config.m4"
INVOKER = ROOT / "tests/native/control-flow/bridge/invoke_dump.php"


class ExtensionContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.extension = EXTENSION.read_text(encoding="utf-8")
        cls.stub = STUB.read_text(encoding="utf-8")
        cls.invoker = INVOKER.read_text(encoding="utf-8")

    def test_config_has_no_w04_stub(self) -> None:
        self.assertNotIn("w04_entry_stub", CONFIG.read_text(encoding="utf-8"))

    def test_wave_three_is_default_and_supported_versions_are_explicit(self) -> None:
        self.assertIn("state->wave = 3;", self.extension)
        self.assertRegex(
            self.extension,
            re.compile(
                r"zend_string_equals_literal\(key, \"wave\"\).*?"
                r"Z_LVAL_P\(value\) != 3 && Z_LVAL_P\(value\) != 4\s*"
                r"&& Z_LVAL_P\(value\) != 5",
                re.DOTALL,
            ),
        )
        self.assertIn("wave?: 3|4|5", self.stub)

    def test_stub_annotations_are_generator_compatible(self) -> None:
        self.assertIn("@return array", self.stub)
        self.assertIn("@param array $options", self.stub)
        self.assertNotIn("@return array{", self.stub)
        self.assertNotIn("@param array{", self.stub)

    def test_w03_schema_has_no_new_field(self) -> None:
        self.assertRegex(
            self.extension,
            re.compile(
                r'if \(state->wave >= 4\) \{\s*'
                r'add_assoc_long\(return_value, "wave", state->wave\);\s*\}',
                re.DOTALL,
            ),
        )
    def test_w04_path_calls_only_the_integrated_wrapper(self) -> None:
        match = re.search(
            r"static bool native_mir_test_lower_w04_and_dump.*?^}",
            self.extension,
            re.DOTALL | re.MULTILINE,
        )
        self.assertIsNotNone(match)
        body = match.group(0)
        self.assertEqual(body.count("zend_mir_lower_w04_zend_op_array("), 1)
        self.assertNotIn("zend_mir_lower_w04_zend_source(", body)
        self.assertNotRegex(body, r"\bzend_mir_(?:lower_source|emit|append|add)_")
        self.assertIn("zend_mir_lowering_result_is_w04_failure_atomic", self.extension)

    def test_w04_stage2_checks_scalar_descriptors(self) -> None:
        self.assertIn("native_mir_test_verify_w04_scalar", self.extension)
        self.assertIn("zend_mir_scalar_descriptor_at", self.extension)
        self.assertIn("native_mir_test_scalar_requirement_matches", self.extension)
        self.assertRegex(
            self.extension,
            re.compile(
                r"return state->wave >= 4\s*"
                r"\? native_mir_test_verify_w04_scalar\(state, view\)\s*"
                r": zend_mir_verify_w03_scalar\(view, diagnostics\);",
                re.DOTALL,
            ),
        )

    def test_protected_regions_reject_before_ssa(self) -> None:
        self.assertRegex(
            self.extension,
            re.compile(
                r"state->wave >= 4 && state->selected->last_try_catch != 0.*?"
                r'"MIRL0015"',
                re.DOTALL,
            ),
        )

    def test_compile_dump_never_executes_source(self) -> None:
        self.assertIn("ZEND_COMPILE_WITHOUT_EXECUTION", self.extension)
        forbidden_native = re.compile(
            r"\b(?:zend_execute|execute_ex|zend_vm_call_opcode_handler|"
            r"mir_interpret|mir_evaluate)\b",
            re.IGNORECASE,
        )
        self.assertIsNone(forbidden_native.search(self.extension))
        self.assertIsNone(
            re.search(
                r"\b(?:eval|include|include_once|require|require_once)\s*[\s(]",
                self.invoker,
                re.IGNORECASE,
            )
        )

    def test_cleanup_and_bailout_boundaries_remain_present(self) -> None:
        for token in (
            "zend_try {",
            "} zend_catch {",
            "native_mir_test_cleanup(state);",
            "zend_clear_exception();",
            "smart_str_free(&state->dump);",
            "zend_arena_destroy(state->ssa_arena);",
        ):
            self.assertIn(token, self.extension)


if __name__ == "__main__":
    unittest.main()
