from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[4]
FRONTEND = ROOT / "Zend" / "Native" / "Lowering" / "Frontend"
TEST_DIR = Path(__file__).resolve().parent


class FrontendContractTests(unittest.TestCase):
    def test_strict_native_suite(self) -> None:
        subprocess.run(
            [
                sys.executable,
                str(TEST_DIR / "run_frontend_tests.py"),
                "--cc",
                "cc",
            ],
            cwd=ROOT,
            check=True,
        )

    def test_required_deliverables_exist(self) -> None:
        required = {
            "zend_mir_zend_source.c",
            "zend_mir_zend_source.h",
            "zend_mir_operand_map.c",
            "zend_mir_value_facts.c",
            "zend_mir_literal_pool.c",
            "zend_mir_slot_map.c",
            "zend_mir_source_positions.c",
        }
        self.assertTrue(required.issubset({path.name for path in FRONTEND.iterdir()}))

    def test_frontend_uses_numeric_opcode_contracts(self) -> None:
        source = (FRONTEND / "zend_mir_operand_map.c").read_text(encoding="utf-8")
        self.assertNotIn("zend_get_opcode_name", source)
        self.assertNotIn("strcmp(", source)
        self.assertIn("case ZEND_QM_ASSIGN:", source)

    def test_opcode_scope_matches_frozen_w03_profile(self) -> None:
        source = (FRONTEND / "zend_mir_operand_map.c").read_text(encoding="utf-8")
        profile = json.loads(
            (ROOT / "docs/native-engine/lowering/w03-opcode-profile.json").read_text(
                encoding="utf-8"
            )
        )
        supported_start = source.index(
            "static bool zend_mir_frontend_opcode_is_supported"
        )
        supported_end = source.index(
            "static bool zend_mir_frontend_is_scalar_source_type"
        )
        supported_block = source[supported_start:supported_end]
        supported = set(re.findall(r"case (ZEND_[A-Z0-9_]+):", supported_block))
        expected_supported = {
            item["opcode"]
            for item in profile["opcodes"]
            if item["classification"] != "deferred"
        }
        self.assertEqual(supported, expected_supported)

        deferred_start = source.index(
            "static zend_mir_lowering_diagnostic_code "
            "zend_mir_frontend_deferred_code"
        )
        deferred_end = source.index(
            "zend_mir_lowering_status zend_mir_frontend_validate_opcode_scope"
        )
        deferred_block = source[deferred_start:deferred_end]
        mappings: dict[str, str] = {}
        pending: list[str] = []
        for line in deferred_block.splitlines():
            case = re.search(r"case (ZEND_[A-Z0-9_]+):", line)
            if case:
                pending.append(case.group(1))
            result = re.search(r"return (ZEND_MIRL_[A-Z0-9_]+);", line)
            if result:
                mappings.update(dict.fromkeys(pending, result.group(1)))
                pending.clear()

        expected_codes = {
            "W04": "ZEND_MIRL_W04_CONTROL_FLOW_DEFERRED",
            "W05": "ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED",
            "W06": "ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED",
            "W07": "ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED",
            "W08": "ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED",
            "W11": "ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED",
        }
        for item in profile["opcodes"]:
            if item["classification"] == "deferred":
                self.assertEqual(
                    mappings[item["opcode"]],
                    expected_codes[item["deferred_wave"]],
                    item["opcode"],
                )

    def test_public_records_do_not_include_zend_layout_headers(self) -> None:
        header = (FRONTEND / "zend_mir_zend_source.h").read_text(encoding="utf-8")
        self.assertNotIn("zend_compile.h", header)
        self.assertNotIn("zend_ssa.h", header)
        self.assertIn("struct _zend_op_array;", header)
        self.assertIn("struct _zend_ssa;", header)


if __name__ == "__main__":
    unittest.main()
