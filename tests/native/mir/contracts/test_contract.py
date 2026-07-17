from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[4]
CHECKER_PATH = ROOT / "scripts" / "native" / "mir" / "check-contract.py"
sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("znmir_contract_checker", CHECKER_PATH)
assert SPEC is not None and SPEC.loader is not None
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)


class ContractTests(unittest.TestCase):
    def test_frozen_contract_passes(self) -> None:
        CHECKER.validate_sources()

    def test_headers_and_fixture_compile_as_c_and_cxx(self) -> None:
        CHECKER.compile_contract()

    def test_duplicate_catalog_id_is_rejected(self) -> None:
        source = (ROOT / "Zend" / "Native" / "MIR" / "zend_mir_effects.h").read_text(encoding="utf-8")
        mutated = source.replace('X(WRITE_MEMORY, "write_memory", 1)',
                                 'X(WRITE_MEMORY, "write_memory", 0)', 1)
        with self.assertRaisesRegex(CHECKER.ContractError, "unique and contiguous"):
            CHECKER.validate_catalog_text(
                mutated,
                "ZEND_MIR_EFFECT_CATALOG",
                list(CHECKER.load_json(CHECKER.EFFECT_MODEL)["catalog"]["effects"]),
            )

    def test_missing_w01_catalog_entry_is_rejected(self) -> None:
        source = (ROOT / "Zend" / "Native" / "MIR" / "zend_mir_effects.h").read_text(encoding="utf-8")
        mutated = source.replace('\tX(TERMINATE, "terminate", 14)', "", 1)
        with self.assertRaises(CHECKER.ContractError):
            CHECKER.validate_catalog_text(
                mutated,
                "ZEND_MIR_EFFECT_CATALOG",
                list(CHECKER.load_json(CHECKER.EFFECT_MODEL)["catalog"]["effects"]),
            )

    def test_reordered_w01_guard_fact_is_rejected(self) -> None:
        source = (ROOT / "Zend" / "Native" / "MIR" / "zend_mir_effects.h").read_text(encoding="utf-8")
        first = '\tX(VALUE_TYPE, "value_type", 0) \\\n'
        second = '\tX(OBJECT_CLASS_IDENTITY, "object_class_identity", 1) \\\n'
        mutated = source.replace(first + second, second + first, 1)
        with self.assertRaisesRegex(CHECKER.ContractError, "does not exactly match W01 order"):
            CHECKER.validate_catalog_text(
                mutated,
                "ZEND_MIR_GUARD_FACT_CATALOG",
                list(CHECKER.load_json(CHECKER.EFFECT_MODEL)["catalog"]["guard_facts"]),
            )

    def test_forbidden_target_include_is_rejected(self) -> None:
        with self.assertRaisesRegex(CHECKER.ContractError, "forbidden target/TPDE"):
            CHECKER.validate_includes_text('#include "tpde/Target/X86.h"\n', "mutated.h")

    def test_target_dependent_field_is_rejected(self) -> None:
        for source in (
            "uint32_t machine_offset;",
            "uint32_t x86_register;",
            "uint32_t aarch64_reg;",
            "tpde_value value;",
        ):
            with self.subTest(source=source), self.assertRaisesRegex(
                CHECKER.ContractError, "target-dependent"
            ):
                CHECKER.validate_target_neutral_fields(source)

    def test_raw_pointer_in_serializable_record_is_rejected(self) -> None:
        source = "typedef struct _example { uint32_t id; void *payload; } example;"
        with self.assertRaisesRegex(CHECKER.ContractError, "raw pointer"):
            CHECKER.validate_serializable_text(source, ("example",))

    def test_changed_invalid_sentinel_is_rejected(self) -> None:
        source = (ROOT / "Zend" / "Native" / "MIR" / "zend_mir_ids.h").read_text(encoding="utf-8")
        mutated = source.replace("UINT32_C(0xffffffff)", "UINT32_C(0xfffffffe)", 1)
        with self.assertRaisesRegex(CHECKER.ContractError, "ZEND_MIR_ID_INVALID"):
            CHECKER.validate_ids_text(mutated)

    def test_changed_enum_sentinel_is_rejected(self) -> None:
        sources = "\n".join(
            (ROOT / "Zend" / "Native" / "MIR" / name).read_text(encoding="utf-8")
            for name in CHECKER.HEADERS
        )
        mutated = sources.replace("ZEND_MIR_OPCODE_INVALID = UINT32_MAX",
                                  "ZEND_MIR_OPCODE_INVALID = 10", 1)
        with self.assertRaisesRegex(CHECKER.ContractError, "ZEND_MIR_OPCODE_INVALID"):
            CHECKER.validate_enum_sentinels(mutated)


if __name__ == "__main__":
    unittest.main()
