import os
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]


class W06BridgeContractTests(unittest.TestCase):
    def test_bridge_exposes_wave_six(self):
        source = (ROOT / "ext/native_mir_test/native_mir_test.c").read_text()
        self.assertIn("state->wave == 6", source)
        self.assertIn("zend_mir_lower_w06_zend_op_array", source)

    def test_bridge_exposes_every_required_atomic_fault(self):
        bridge = (ROOT / "ext/native_mir_test/native_mir_test.c").read_text()
        runner = (
            ROOT / "scripts/native/values/run-w06-differential.py"
        ).read_text()
        for fault in (
            "compile_bailout",
            "finalize_failure",
            "structural_verifier_failure",
            "scalar_verifier_failure",
            "control_flow_verifier_failure",
            "call_verifier_failure",
            "fingerprint_recompute_failure",
            "value_inventory",
            "value_plan",
            "value_storage",
            "value_reference",
            "value_alias",
            "value_event",
            "value_separation",
            "value_call_transfer",
            "value_verifier_failure",
            "dump_failure",
        ):
            self.assertIn(f'"{fault}"', bridge)
            self.assertIn(f'"{fault}"', runner)
        self.assertNotIn("-d extension=native_mir_test", runner)

    def test_generated_config_contains_value_pipeline(self):
        config = (ROOT / "ext/native_mir_test/config.m4").read_text()
        for source in (
            "zend_mir_verify_values.c",
            "zend_mir_value_core.c",
            "zend_mir_value_lowering.c",
        ):
            self.assertIn(source, config)

    def test_call_transfer_model_is_scalable(self):
        source = (
            ROOT
            / "Zend/Native/Values/Lowering/zend_mir_value_lowering.c"
        ).read_text()
        self.assertIn("transfer->parameter_modes = target.parameter_modes", source)
        self.assertIn(
            "producer.result_ssa_variable_id\n"
            "\t\t\t\t\t\t\t== argument.value_ssa_variable_id",
            source,
        )
        self.assertNotIn("uint64_t parameter_mode", source)

    def test_call_return_type_evidence_stays_process_local(self):
        frontend = (
            ROOT / "Zend/Native/Lowering/Frontend/zend_mir_zend_source.c"
        ).read_text()
        header = (
            ROOT / "Zend/Native/Lowering/Frontend/zend_mir_zend_source.h"
        ).read_text()
        self.assertIn("zend_mir_zend_source_w06_call_return_type", frontend)
        self.assertIn("no zend_function pointer is", header)
        self.assertNotIn("const zend_function *", header)

    def test_real_candidate_corpus_and_faults(self):
        candidate = os.environ.get("W06_CANDIDATE_PHP")
        if not candidate:
            self.skipTest("W06_CANDIDATE_PHP is not set")
        completed = subprocess.run(
            [
                "python3",
                "scripts/native/values/run-w06-differential.py",
                "--candidate-php",
                candidate,
            ],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("W06 differential self-test: ok", completed.stdout)


if __name__ == "__main__":
    unittest.main()
