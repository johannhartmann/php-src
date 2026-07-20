from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[4]
FIXTURES = Path(__file__).with_name("value-contract-fixtures.json")
PROFILE = ROOT / "docs/native-engine/values/w06-opcode-profile.json"
TRANSITIONS = ROOT / "docs/native-engine/values/w06-transition-profile.json"
GENERATOR = ROOT / "scripts/native/values/generate-w06-profile.py"


class W06ContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.fixtures = json.loads(FIXTURES.read_text(encoding="utf-8"))
        cls.profile = json.loads(PROFILE.read_text(encoding="utf-8"))
        cls.transitions = json.loads(TRANSITIONS.read_text(encoding="utf-8"))

    def test_live_profile_is_exact_and_generated(self) -> None:
        spec = importlib.util.spec_from_file_location("w06_generator_test", GENERATOR)
        self.assertIsNotNone(spec)
        self.assertIsNotNone(spec.loader)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        generated, _ = module.build()
        self.assertEqual(self.profile, generated)
        self.assertEqual(0, self.profile["unresolved_w06_count"])
        self.assertFalse(any(
            entry["later_wave"] == "W06" for entry in self.profile["opcodes"]
        ))

    def test_required_operations_and_refcounted_variants(self) -> None:
        by_name = {entry["opcode"]: entry for entry in self.profile["opcodes"]}
        for name in (
            "ZEND_ASSIGN", "ZEND_ASSIGN_REF", "ZEND_MAKE_REF", "ZEND_COPY_TMP",
            "ZEND_SEPARATE", "ZEND_UNSET_CV", "ZEND_ISSET_ISEMPTY_CV",
            "ZEND_CHECK_VAR", "ZEND_RETURN_BY_REF", "ZEND_SEND_REF",
            "ZEND_SEND_VAR_NO_REF", "ZEND_SEND_VAR_NO_REF_EX",
            "ZEND_SEND_FUNC_ARG", "ZEND_CHECK_FUNC_ARG",
        ):
            self.assertEqual("accepted", by_name[name]["decision"], name)
        for name in ("ZEND_QM_ASSIGN", "ZEND_FREE", "ZEND_RETURN"):
            self.assertEqual("accepted_variant", by_name[name]["decision"], name)

    def test_required_later_wave_buckets_are_explicit(self) -> None:
        by_name = {entry["opcode"]: entry for entry in self.profile["opcodes"]}
        expected = {
            "ZEND_FETCH_DIM_R": "W07",
            "ZEND_CONCAT": "W07",
            "ZEND_FETCH_OBJ_R": "W08",
            "ZEND_VERIFY_RETURN_TYPE": "W09",
            "ZEND_BIND_GLOBAL": "W10",
            "ZEND_ECHO": "W15",
            "ZEND_DIV": "W16",
        }
        for name, wave in expected.items():
            self.assertEqual(wave, by_name[name]["later_wave"], name)
        self.assertEqual("owner_sequence", by_name["ZEND_OP_DATA"]["decision"])

    def test_alias_and_phi_merges_are_conservative(self) -> None:
        for case in self.fixtures["alias_cases"]:
            if case["left_cell"] == case["right_cell"]:
                actual = "must_alias"
            elif case["fresh_proof"]:
                actual = "no_alias"
            else:
                actual = "may_alias"
            self.assertEqual(case["expected"], actual)
        self.assertEqual(
            "may_alias", self.transitions["alias_merge"]["unknown_or_conflicting"]
        )
        phi = {item["left"] + "+" + item["right"]: item["expected"]
               for item in self.fixtures["phi_merges"]}
        self.assertEqual("shared", phi["unique+shared"])
        self.assertEqual("invalid_without_canonicalization", phi["direct+reference"])

    def test_reference_and_indirect_are_distinct(self) -> None:
        header = (ROOT / "Zend/Native/MIR/zend_mir_values.h").read_text(encoding="utf-8")
        self.assertIn("ZEND_MIR_STORAGE_REFERENCE = 2", header)
        self.assertIn("ZEND_MIR_STORAGE_INDIRECT = 3", header)
        self.assertIn("reference_cell_id", header)
        self.assertIn("indirect_target_id", header)

    def test_transitions_and_invalid_records_are_complete(self) -> None:
        actions = [item["action"] for item in self.transitions["transitions"]]
        self.assertEqual(
            ["borrow", "copy_addref", "move", "release",
             "transfer_to_callee", "transfer_from_callee"],
            actions,
        )
        self.assertTrue(
            set(self.fixtures["invalid_records"][:-1])
            <= set(self.transitions["invalid_transitions"])
        )

    def test_separation_never_executes_clone(self) -> None:
        self.assertFalse(self.transitions["separation"]["clone_execution"])
        for case in self.fixtures["separation_cases"]:
            valid = not (
                case["required"] == "yes"
                and case["source_payload"] == case["result_payload"]
            )
            self.assertEqual(case["valid"], valid)

    def test_parameter_modes_scale_beyond_64(self) -> None:
        self.assertIn(65, self.fixtures["parameter_ordinals"])
        self.assertIn(128, self.fixtures["parameter_ordinals"])
        self.assertGreaterEqual(
            self.transitions["call_transfer"]["parameter_ordinal_max"], 128
        )
        self.assertEqual("span", self.transitions["call_transfer"]["parameter_mode_storage"])

    def test_c11_and_cpp20_contract_fixture(self) -> None:
        cc = shutil.which("cc")
        cxx = shutil.which("c++")
        self.assertIsNotNone(cc)
        self.assertIsNotNone(cxx)
        fixture = Path(__file__).with_name("test_contract.c")
        subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I.", "-fsyntax-only", str(fixture)],
            cwd=ROOT, check=True,
        )
        subprocess.run(
            [cxx, "-x", "c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
             "-pedantic", "-I.", "-fsyntax-only", str(fixture)],
            cwd=ROOT, check=True,
        )

    def test_contract_checker(self) -> None:
        subprocess.run(
            [sys.executable, str(ROOT / "scripts/native/values/check-contract.py"), "--check"],
            cwd=ROOT, check=True,
        )


if __name__ == "__main__":
    unittest.main()
