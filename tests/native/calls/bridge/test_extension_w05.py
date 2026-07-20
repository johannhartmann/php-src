from __future__ import annotations

import os
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
EXTENSION = ROOT / "ext/native_mir_test/native_mir_test.c"
STUB = ROOT / "ext/native_mir_test/native_mir_test.stub.php"
CONFIG = ROOT / "ext/native_mir_test/config.m4"


class ExtensionW05Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.extension = EXTENSION.read_text(encoding="utf-8")

    def test_wave_five_is_explicit_and_default_remains_wave_three(self) -> None:
        self.assertIn("state->wave = 3;", self.extension)
        self.assertIn("Z_LVAL_P(value) != 5", self.extension)
        self.assertIn("wave?: 3|4|5", STUB.read_text(encoding="utf-8"))

    def test_compile_dump_never_executes_source_or_mir(self) -> None:
        self.assertIn("ZEND_COMPILE_WITHOUT_EXECUTION", self.extension)
        forbidden = re.compile(
            r"\b(?:zend_execute|execute_ex|zend_vm_call_opcode_handler|"
            r"mir_interpret|mir_evaluate)\b",
            re.IGNORECASE,
        )
        self.assertIsNone(forbidden.search(self.extension))

    def test_w05_uses_only_the_integrated_wrapper(self) -> None:
        match = re.search(
            r"static bool native_mir_test_lower_w05_and_dump.*?^}",
            self.extension,
            re.DOTALL | re.MULTILINE,
        )
        self.assertIsNotNone(match)
        body = match.group(0)
        self.assertEqual(body.count("zend_mir_lower_w05_zend_op_array("), 1)
        self.assertNotIn("zend_mir_lower_w05_zend_source(", body)
        self.assertIn("zend_mir_lowering_result_is_w05_failure_atomic", body)

    def test_config_is_generated_from_the_global_manifest(self) -> None:
        config = CONFIG.read_text(encoding="utf-8")
        self.assertEqual(config.count("BEGIN GENERATED NATIVE SOURCES"), 1)
        self.assertEqual(config.count("END GENERATED NATIVE SOURCES"), 1)
        self.assertIn(
            "PHP_ADD_SOURCES_X([Zend/Native/Calls/Model], "
            "[zend_mir_call_model.c], "
            "[-DZEND_MIR_W05_TEST_FAULTS=1], [PHP_GLOBAL_OBJS])",
            config,
        )


class RealCandidateTests(unittest.TestCase):
    def test_real_candidate_models_a_direct_user_call(self) -> None:
        candidate = os.environ.get("TEST_PHP_EXECUTABLE")
        if not candidate:
            self.skipTest("TEST_PHP_EXECUTABLE is not set")
        import importlib.util

        specification = importlib.util.spec_from_file_location(
            "w05_real_dump", ROOT / "scripts/native/calls/dump-w05.py"
        )
        assert specification is not None and specification.loader is not None
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        source = (
            b"<?php function w05_target(): void { echo 1; } "
            b"function w05_case(): void { w05_target(); }"
        )
        document = module.invoke(
            module.candidate_path(candidate),
            source,
            "real-candidate.php",
            "w05_case",
            repeat=2,
        )
        first, second = document["calls"]
        self.assertEqual(first, second)
        self.assertEqual(first["status"], "accepted")
        self.assertIn("opcode call_direct_user", first["mir"])
        self.assertIn("codegen-eligible false", first["mir"])

    def test_real_candidate_preserves_zero_based_arguments_and_frames(self) -> None:
        candidate = os.environ.get("TEST_PHP_EXECUTABLE")
        if not candidate:
            self.skipTest("TEST_PHP_EXECUTABLE is not set")
        import importlib.util

        specification = importlib.util.spec_from_file_location(
            "w05_real_frame_dump", ROOT / "scripts/native/calls/dump-w05.py"
        )
        assert specification is not None and specification.loader is not None
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        source = (
            b"<?php function w05_frame_target($left, $right): void "
            b"{ echo $left; } function w05_frame_case(): void "
            b"{ w05_frame_target(7, true); }"
        )
        result = module.invoke(
            module.candidate_path(candidate),
            source,
            "real-frame-candidate.php",
            "w05_frame_case",
        )["calls"][0]
        mir = result["mir"]
        self.assertEqual(result["status"], "accepted")
        self.assertIn("call-argument ca0 site cs0 ordinal 0", mir)
        self.assertIn("call-argument ca1 site cs0 ordinal 1", mir)
        self.assertIn("kind pending_call", mir)
        self.assertIn("safepoint user_call canonical true", mir)
        self.assertIn("safepoint function_entry canonical true", mir)
        self.assertIn("continuations 0+4", mir)
        self.assertRegex(
            mir,
            r"caller frame fs\d+ function f0 symbol s0 op-array oa0",
        )
        self.assertIn(
            "callee frame invalid function invalid symbol s1 "
            "op-array oa1",
            mir,
        )
        self.assertNotRegex(mir, r"frame fs\d+ function f\d+ parent fs\d+")

    def test_no_discard_by_name_finish_is_deferred_to_w10(self) -> None:
        candidate = os.environ.get("TEST_PHP_EXECUTABLE")
        if not candidate:
            self.skipTest("TEST_PHP_EXECUTABLE is not set")
        import importlib.util

        specification = importlib.util.spec_from_file_location(
            "w05_no_discard_dump", ROOT / "scripts/native/calls/dump-w05.py"
        )
        assert specification is not None and specification.loader is not None
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        source = (
            b"<?php #[\\NoDiscard] function w05_target(): int { return 1; } "
            b"function w05_case(): void { w05_target(); }"
        )
        result = module.invoke(
            module.candidate_path(candidate),
            source,
            "no-discard-finish.php",
            "w05_case",
        )["calls"][0]
        self.assertEqual(result["status"], "rejected")
        self.assertIn(
            "MIRL0023",
            {diagnostic["code"] for diagnostic in result["diagnostics"]},
        )

    def test_parameter_65_by_reference_is_deferred_to_w06(self) -> None:
        candidate = os.environ.get("TEST_PHP_EXECUTABLE")
        if not candidate:
            self.skipTest("TEST_PHP_EXECUTABLE is not set")
        import importlib.util

        specification = importlib.util.spec_from_file_location(
            "w05_parameter_boundary_dump",
            ROOT / "scripts/native/calls/dump-w05.py",
        )
        assert specification is not None and specification.loader is not None
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        source = (
            ROOT
            / "tests/native/calls/corpus/cases/by_ref_parameter_65.php"
        ).read_bytes()
        result = module.invoke(
            module.candidate_path(candidate),
            source,
            "by-ref-parameter-65.php",
            "w05_case",
        )["calls"][0]
        self.assertEqual(result["status"], "rejected")
        self.assertIn(
            "MIRL0024",
            {diagnostic["code"] for diagnostic in result["diagnostics"]},
        )

    def test_normalized_named_argument_is_deferred_to_w07(self) -> None:
        candidate = os.environ.get("TEST_PHP_EXECUTABLE")
        if not candidate:
            self.skipTest("TEST_PHP_EXECUTABLE is not set")
        import importlib.util

        specification = importlib.util.spec_from_file_location(
            "w05_normalized_named_dump",
            ROOT / "scripts/native/calls/dump-w05.py",
        )
        assert specification is not None and specification.loader is not None
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        source = (
            ROOT
            / (
                "tests/native/calls/corpus/cases/"
                "named_argument_normalized_position.php"
            )
        ).read_bytes()
        result = module.invoke(
            module.candidate_path(candidate),
            source,
            "named-argument-normalized-position.php",
            "w05_case",
        )["calls"][0]
        self.assertEqual(result["status"], "rejected")
        self.assertIn(
            "MIRL0024",
            {diagnostic["code"] for diagnostic in result["diagnostics"]},
        )

    def test_64_by_value_parameters_remain_supported(self) -> None:
        candidate = os.environ.get("TEST_PHP_EXECUTABLE")
        if not candidate:
            self.skipTest("TEST_PHP_EXECUTABLE is not set")
        import importlib.util

        specification = importlib.util.spec_from_file_location(
            "w05_parameter_limit_dump",
            ROOT / "scripts/native/calls/dump-w05.py",
        )
        assert specification is not None and specification.loader is not None
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        source = (
            ROOT
            / "tests/native/calls/corpus/cases/by_value_parameter_64.php"
        ).read_bytes()
        result = module.invoke(
            module.candidate_path(candidate),
            source,
            "by-value-parameter-64.php",
            "w05_case",
        )["calls"][0]
        self.assertEqual(result["status"], "accepted")

    def test_proved_user_do_fcall_has_real_source_opcode_evidence(self) -> None:
        candidate = os.environ.get("TEST_PHP_EXECUTABLE")
        if not candidate:
            self.skipTest("TEST_PHP_EXECUTABLE is not set")
        import importlib.util

        specification = importlib.util.spec_from_file_location(
            "w05_do_fcall_dump", ROOT / "scripts/native/calls/dump-w05.py"
        )
        assert specification is not None and specification.loader is not None
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        source = (
            ROOT
            / "tests/native/calls/corpus/cases/"
            "direct_call_exact_user_do_fcall_proof.php"
        ).read_bytes()
        result = module.invoke(
            module.candidate_path(candidate),
            source,
            "direct-user-do-fcall.php",
            "w05_case",
            compiler_mode="ignore_user_functions",
        )["calls"][0]
        self.assertEqual(result["status"], "accepted")
        self.assertIn("ZEND_INIT_FCALL", result["source_opcodes"])
        self.assertIn("ZEND_DO_FCALL", result["source_opcodes"])
        self.assertNotIn("ZEND_DO_UCALL", result["source_opcodes"])

    def test_exact_scalar_call_results_are_modeled_as_instruction_results(self) -> None:
        candidate = os.environ.get("TEST_PHP_EXECUTABLE")
        if not candidate:
            self.skipTest("TEST_PHP_EXECUTABLE is not set")
        import importlib.util

        specification = importlib.util.spec_from_file_location(
            "w05_scalar_result_dump", ROOT / "scripts/native/calls/dump-w05.py"
        )
        assert specification is not None and specification.loader is not None
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        expected = {
            "int": "i64",
            "bool": "i1",
            "float": "double",
            "null": "zval",
        }
        for kind, representation in expected.items():
            with self.subTest(kind=kind):
                source = (
                    ROOT
                    / "tests/native/calls/corpus/cases"
                    / f"direct_user_scalar_result_{kind}.php"
                ).read_bytes()
                result = module.invoke(
                    module.candidate_path(candidate),
                    source,
                    f"direct-user-scalar-result-{kind}.php",
                    "w05_case",
                )["calls"][0]
                self.assertEqual(result["status"], "accepted")
                self.assertRegex(
                    result["mir"],
                    rf"opcode call_direct_user representation {representation} "
                    rf"result v\d+",
                )
                self.assertRegex(result["mir"], r"call-site cs0 .* result v\d+")

    def test_scalar_call_result_can_feed_followup_scalar_lowering(self) -> None:
        candidate = os.environ.get("TEST_PHP_EXECUTABLE")
        if not candidate:
            self.skipTest("TEST_PHP_EXECUTABLE is not set")
        import importlib.util

        specification = importlib.util.spec_from_file_location(
            "w05_scalar_followup_dump", ROOT / "scripts/native/calls/dump-w05.py"
        )
        assert specification is not None and specification.loader is not None
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        source = (
            ROOT
            / "tests/native/calls/corpus/cases/"
            "direct_user_scalar_result_followup.php"
        ).read_bytes()
        result = module.invoke(
            module.candidate_path(candidate),
            source,
            "direct-user-scalar-result-followup.php",
            "w05_case",
        )["calls"][0]
        self.assertEqual(result["status"], "accepted")
        self.assertIn("opcode call_direct_user", result["mir"])
        self.assertIn("opcode i64_add_no_overflow", result["mir"])

    def test_default_bearing_target_is_deferred_to_w07(self) -> None:
        candidate = os.environ.get("TEST_PHP_EXECUTABLE")
        if not candidate:
            self.skipTest("TEST_PHP_EXECUTABLE is not set")
        import importlib.util

        specification = importlib.util.spec_from_file_location(
            "w05_default_argument_dump", ROOT / "scripts/native/calls/dump-w05.py"
        )
        assert specification is not None and specification.loader is not None
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        source = (
            ROOT / "tests/native/calls/corpus/cases/default_argument.php"
        ).read_bytes()
        result = module.invoke(
            module.candidate_path(candidate),
            source,
            "default-argument.php",
            "w05_case",
        )["calls"][0]
        self.assertEqual(result["status"], "rejected")
        self.assertIn(
            "MIRL0025",
            {diagnostic["code"] for diagnostic in result["diagnostics"]},
        )

    def test_recursive_self_call_reuses_caller_identity_and_is_rejected(self) -> None:
        candidate = os.environ.get("TEST_PHP_EXECUTABLE")
        if not candidate:
            self.skipTest("TEST_PHP_EXECUTABLE is not set")
        import importlib.util

        specification = importlib.util.spec_from_file_location(
            "w05_recursive_dump", ROOT / "scripts/native/calls/dump-w05.py"
        )
        assert specification is not None and specification.loader is not None
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        source = (
            ROOT / "tests/native/calls/corpus/cases/recursive_self_call.php"
        ).read_bytes()
        result = module.invoke(
            module.candidate_path(candidate),
            source,
            "recursive-self-call.php",
            "w05_case",
        )["calls"][0]
        self.assertEqual(result["status"], "rejected")
        self.assertIn(
            "MIRL0023",
            {diagnostic["code"] for diagnostic in result["diagnostics"]},
        )

    def test_call_instruction_preserves_source_opline_order_after_phi(self) -> None:
        candidate = os.environ.get("TEST_PHP_EXECUTABLE")
        if not candidate:
            self.skipTest("TEST_PHP_EXECUTABLE is not set")
        import importlib.util

        specification = importlib.util.spec_from_file_location(
            "w05_source_order_dump", ROOT / "scripts/native/calls/dump-w05.py"
        )
        assert specification is not None and specification.loader is not None
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        source = (
            ROOT / "tests/native/calls/corpus/cases/direct_call_after_phi.php"
        ).read_bytes()
        result = module.invoke(
            module.candidate_path(candidate),
            source,
            "direct-call-after-phi.php",
            "w05_case",
        )["calls"][0]
        self.assertEqual(result["status"], "accepted")
        by_block: dict[str, list[int]] = {}
        for line in result["mir"].splitlines():
            match = re.match(r"instruction i\d+ block (b\d+) .* source p(\d+)$", line)
            if match:
                by_block.setdefault(match.group(1), []).append(int(match.group(2)))
        self.assertTrue(by_block)
        for positions in by_block.values():
            self.assertEqual(positions, sorted(positions))

    def test_w05_module_oom_is_failure_atomic(self) -> None:
        candidate = os.environ.get("TEST_PHP_EXECUTABLE")
        if not candidate:
            self.skipTest("TEST_PHP_EXECUTABLE is not set")
        import importlib.util

        specification = importlib.util.spec_from_file_location(
            "w05_real_oom_dump", ROOT / "scripts/native/calls/dump-w05.py"
        )
        assert specification is not None and specification.loader is not None
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        source = (
            b"<?php function w05_oom_target(): void { echo 1; } "
            b"function w05_oom_case(): void { w05_oom_target(); }"
        )
        document = module.invoke(
            module.candidate_path(candidate),
            source,
            "real-oom-candidate.php",
            "w05_oom_case",
            repeat=3,
            fault="module_oom",
        )
        for result in document["calls"]:
            self.assertEqual(result["status"], "error")
            self.assertEqual(result["phase"], "lowering")
            self.assertIsNone(result["mir"])
            self.assertIn(
                "MIRL0001",
                {diagnostic["code"] for diagnostic in result["diagnostics"]},
            )

    def test_w05_stage_faults_are_failure_atomic(self) -> None:
        candidate = os.environ.get("TEST_PHP_EXECUTABLE")
        if not candidate:
            self.skipTest("TEST_PHP_EXECUTABLE is not set")
        import importlib.util

        specification = importlib.util.spec_from_file_location(
            "w05_real_stage_fault_dump",
            ROOT / "scripts/native/calls/dump-w05.py",
        )
        assert specification is not None and specification.loader is not None
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        source = (
            b"<?php function w05_fault_target(int $value): int { "
            b"return $value; } function w05_fault_case(): int { "
            b"return w05_fault_target(7); }"
        )
        expected = {
            "planner_allocation": "MIRL0028",
            "target_snapshot": "MIRL0028",
            "argument_table": "MIRL0028",
            "frame_state": "MIRL0028",
            "call_record": "MIRL0028",
            "finalize_failure": "MIRL0028",
            "stage1_verifier_failure": "MIRL0009",
            "stage2_verifier_failure": "MIRL0010",
            "call_verifier_failure": "MIRL0029",
        }
        for fault, code in expected.items():
            with self.subTest(fault=fault):
                result = module.invoke(
                    module.candidate_path(candidate),
                    source,
                    f"real-{fault}.php",
                    "w05_fault_case",
                    fault=fault,
                )["calls"][0]
                self.assertEqual(result["status"], "error")
                self.assertIn(result["phase"], {"lowering", "verify"})
                self.assertIsNone(result["mir"])
                self.assertIn(
                    code,
                    {
                        diagnostic["code"]
                        for diagnostic in result["diagnostics"]
                    },
                )


if __name__ == "__main__":
    unittest.main()
