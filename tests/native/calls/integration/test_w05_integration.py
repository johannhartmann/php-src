"""W05 call-model integration tests."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[4]


def load_script(name: str, path: str):
    specification = importlib.util.spec_from_file_location(name, ROOT / path)
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


validator = load_script("w05_validate", "scripts/native/calls/validate-w05.py")
call_tests = load_script("w05_test", "scripts/native/calls/test-w05.py")


class W05IntegrationTest(unittest.TestCase):
    def test_source_wiring_decisions_and_goldens(self) -> None:
        validator.validate()

    def test_goldens_are_raw_complete_pipeline_output(self) -> None:
        index = json.loads(validator.GOLDENS.read_text(encoding="utf-8"))
        self.assertFalse(index["normalization"])
        for entry in index["cases"].values():
            body = (ROOT / entry["path"]).read_text(encoding="utf-8")
            self.assertTrue(body.endswith("end\n"))
            self.assertIn("opcode call_direct_user", body)
            self.assertIn("call-target ", body)
            self.assertIn("call-site ", body)

    def test_final_fault_matrix_covers_each_verifier_facet(self) -> None:
        required = {
            "structural_verifier_failure",
            "scalar_verifier_failure",
            "control_flow_verifier_failure",
            "call_verifier_failure",
            "fingerprint_recompute_failure",
        }
        self.assertTrue(required.issubset(call_tests.FAULTS))
        self.assertTrue(all(call_tests.FAULTS[name][2] == "MIRL0029" for name in required))


if __name__ == "__main__":
    unittest.main()
