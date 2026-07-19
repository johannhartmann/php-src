from __future__ import annotations

import pathlib
import subprocess
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[4]


class ControlFlowRunnerTest(unittest.TestCase):
    def test_strict_c11_unit_suite(self) -> None:
        subprocess.run(
            [
                sys.executable,
                str(pathlib.Path(__file__).with_name("run_control_flow_tests.py")),
                "--cc",
                "cc",
            ],
            cwd=ROOT,
            check=True,
        )

    def test_production_manifest_is_complete(self) -> None:
        required = {
            "Zend/Native/Lowering/ControlFlow/zend_mir_control_flow_proofs.c",
            "Zend/Native/Lowering/ControlFlow/zend_mir_control_flow_provider.c",
            "Zend/Native/Lowering/ControlFlow/zend_mir_lower_control_flow.c",
            "Zend/Native/MIR/ControlFlow/zend_mir_control_flow_map.c",
            "Zend/Native/MIR/ControlFlow/zend_mir_verify_control_flow.c",
        }
        self.assertTrue(all((ROOT / path).is_file() for path in required))


if __name__ == "__main__":
    unittest.main()
