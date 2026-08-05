#!/usr/bin/env python3
"""Contract checks for the test-build-only native VM fallback probes."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class NativeVmProbeContractTest(unittest.TestCase):
    def test_probe_is_private_and_noop_outside_test_build(self) -> None:
        header = (ROOT / "Zend/zend_vm_probe.h").read_text()

        self.assertIn("#ifdef HAVE_NATIVE_MIR_TEST", header)
        self.assertIn(
            "# define ZEND_NATIVE_MIR_TEST_PROBE_VM_HANDLER() do { } while (0)",
            header,
        )
        self.assertIn(
            "# define ZEND_NATIVE_MIR_TEST_PROBE_EXECUTE_EX() do { } while (0)",
            header,
        )
        self.assertIn(
            "# define ZEND_NATIVE_MIR_TEST_PROBE_OPLINE_HANDLER() do { } while (0)",
            header,
        )
        self.assertNotIn("ZEND_API", header)

    def test_probe_is_armed_only_around_native_module_execution(self) -> None:
        bridge = (ROOT / "ext/native_mir_test/native_mir_test.c").read_text()

        wrapper = bridge.index("static bool native_mir_test_execute_module(")
        saved = bridge.index(
            "uint32_t previous_depth = state->vm_probe_depth;", wrapper
        )
        guard = bridge.index(
            "if (UNEXPECTED(previous_depth == UINT32_MAX)) {", saved
        )
        overflow_return = bridge.index("return false;", guard)
        arm = bridge.index(
            "state->vm_probe_depth = previous_depth + 1;", overflow_return
        )
        call = bridge.index(
            "native_mir_test_execute_module_inner(state, arguments);", arm
        )
        disarm = bridge.index(
            "state->vm_probe_depth = previous_depth;", call
        )
        self.assertLess(saved, guard)
        self.assertLess(guard, overflow_return)
        self.assertLess(overflow_return, arm)
        self.assertLess(arm, call)
        self.assertLess(call, disarm)
        self.assertIn("state->vm_probe_depth = 0;", bridge)

    def test_nested_bridge_probes_propagate_to_armed_parents(self) -> None:
        bridge = (ROOT / "ext/native_mir_test/native_mir_test.c").read_text()

        self.assertIn(
            "state->vm_probe_parent = previous_active_state;", bridge
        )
        self.assertEqual(bridge.count("state = state->vm_probe_parent;"), 3)
        self.assertIn("state->vm_probe_parent = NULL;", bridge)

    def test_generated_vm_has_all_three_real_entry_probes(self) -> None:
        template = (ROOT / "Zend/zend_vm_execute.skl").read_text()
        generator = (ROOT / "Zend/zend_vm_gen.php").read_text()
        generated = (ROOT / "Zend/zend_vm_execute.h").read_text()

        self.assertIn("ZEND_NATIVE_MIR_TEST_PROBE_EXECUTE_EX();", template)
        execute_entry = template.index(
            "ZEND_NATIVE_MIR_TEST_PROBE_EXECUTE_EX();"
        )
        self.assertIn(
            "ZEND_NATIVE_MIR_TEST_PROBE_VM_HANDLER();",
            template[:execute_entry],
        )
        self.assertIn("$is_handler=false", generator)
        self.assertIn(
            "ZEND_NATIVE_MIR_TEST_PROBE_OPLINE_HANDLER();", generator
        )
        self.assertIn("ZEND_NATIVE_MIR_TEST_PROBE_VM_HANDLER();", generator)
        self.assertEqual(
            generated.count("ZEND_NATIVE_MIR_TEST_PROBE_EXECUTE_EX();"), 1
        )
        self.assertEqual(
            generated.count("ZEND_NATIVE_MIR_TEST_PROBE_VM_HANDLER();"), 2
        )
        self.assertGreater(
            generated.count("ZEND_NATIVE_MIR_TEST_PROBE_OPLINE_HANDLER();"),
            1000,
        )

    def test_global_native_execute_slot_does_not_use_vm_probe(self) -> None:
        executor = (
            ROOT / "Zend/Native/Compiler/zend_native_executor.c"
        ).read_text()
        start = executor.index(
            "void zend_native_executor_execute_ex(zend_execute_data *execute_data)"
        )
        body = executor[start:]

        self.assertIn(
            "zend_execute_ex = zend_native_executor_execute_ex;", executor
        )
        self.assertNotIn("ZEND_NATIVE_MIR_TEST_PROBE_", body)

    def test_positive_calibration_uses_real_execute_ex(self) -> None:
        bridge = (ROOT / "ext/native_mir_test/native_mir_test.c").read_text()
        start = bridge.index("static bool native_mir_test_calibrate_vm_probes(")
        end = bridge.index("static bool native_mir_test_execute_product(", start)
        calibration = bridge[start:end]

        self.assertIn("execute_ex(frame);", calibration)
        self.assertIn(
            "ZEND_CALL_TOP_FUNCTION | ZEND_CALL_DYNAMIC", calibration
        )
        self.assertIn("state->vm_handler_calls == 0", calibration)
        self.assertIn("state->execute_ex_calls == 0", calibration)
        self.assertIn("state->opline_handler_calls == 0", calibration)
        self.assertIn("zend_try {", calibration)
        self.assertIn("} zend_catch {", calibration)
        self.assertIn("zend_native_execution_cleanup_frame(frame);", calibration)
        self.assertIn("EG(current_execute_data) = previous;", calibration)
        self.assertIn("EG(vm_stack_top) = previous_stack_top;", calibration)
        self.assertIn("zval_ptr_dtor(&result);", calibration)
        self.assertIn("zend_bailout();", calibration)

    def test_arguments_are_validated_before_compilation(self) -> None:
        bridge = (ROOT / "ext/native_mir_test/native_mir_test.c").read_text()
        validation = bridge.index("native_mir_test_validate_arguments(")
        compile_call = bridge.index("if (native_mir_test_compile(state))")
        validation_call = bridge.rindex(
            "native_mir_test_validate_arguments(state, arguments)",
            validation,
            compile_call,
        )

        self.assertLess(validation_call, compile_call)
        self.assertIn("key != NULL || index != expected_index", bridge)
        self.assertNotIn("ZEND_ASSERT(argument != NULL)", bridge)

    def test_phpt_requires_positive_runtime_counts(self) -> None:
        phpt = (
            ROOT / "ext/native_mir_test/tests/w14_vm_probe_contract.phpt"
        ).read_text()

        self.assertIn("$execution['vm_handler_calls'] > 0", phpt)
        self.assertIn("$execution['execute_ex_calls'] > 0", phpt)
        self.assertIn("$execution['opline_handler_calls'] > 0", phpt)
        self.assertIn("positive=yes", phpt)
        self.assertIn("sparse=error named=error", phpt)


if __name__ == "__main__":
    unittest.main()
