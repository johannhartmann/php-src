"""Static checks for the additive W04 test-extension bridge."""

from __future__ import annotations

import hashlib
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
EXTENSION = ROOT / "ext/native_mir_test/native_mir_test.c"
STUB = ROOT / "ext/native_mir_test/native_mir_test.stub.php"
CONFIG = ROOT / "ext/native_mir_test/config.m4"
INVOKER = ROOT / "tests/native/control-flow/bridge/invoke_dump.php"
EXPECTED_CONFIG_SHA256 = (
    "a39f7107fc9e790275f375e94a116465a84db6da7a4bdb18909b70506147e0e9"
)


class ExtensionContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.extension = EXTENSION.read_text(encoding="utf-8")
        cls.stub = STUB.read_text(encoding="utf-8")
        cls.invoker = INVOKER.read_text(encoding="utf-8")

    def test_config_is_unchanged_and_has_no_w04_stub(self) -> None:
        self.assertEqual(
            hashlib.sha256(CONFIG.read_bytes()).hexdigest(),
            EXPECTED_CONFIG_SHA256,
        )
        self.assertNotIn("w04_entry_stub", CONFIG.read_text(encoding="utf-8"))

    def test_wave_three_is_default_and_only_three_or_four_are_valid(self) -> None:
        self.assertIn("state->wave = 3;", self.extension)
        self.assertRegex(
            self.extension,
            re.compile(
                r"zend_string_equals_literal\(key, \"wave\"\).*?"
                r"Z_LVAL_P\(value\) != 3 && Z_LVAL_P\(value\) != 4",
                re.DOTALL,
            ),
        )
        self.assertIn("wave?: 3|4", self.stub)

    def test_w03_schema_has_no_new_field(self) -> None:
        self.assertRegex(
            self.extension,
            re.compile(
                r'if \(state->wave == 4\) \{\s*'
                r'add_assoc_long\(return_value, "wave", 4\);\s*\}',
                re.DOTALL,
            ),
        )
        self.assertNotIn('add_assoc_long(return_value, "wave", state->wave)', self.extension)

    def test_w04_path_calls_only_the_a_owned_wrapper(self) -> None:
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
