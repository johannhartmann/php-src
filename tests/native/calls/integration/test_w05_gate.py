"""Integration-level checks for the W05-v2 gate and seal tooling."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[4]


def load_script(name: str, path: str):
    specification = importlib.util.spec_from_file_location(name, ROOT / path)
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


validator = load_script("w05_validate_gate", "scripts/native/calls/validate-w05.py")
reviews = load_script(
    "w05_build_review_manifest",
    "scripts/native/calls/build-review-manifest.py",
)
seal = load_script("w05_seal_gate", "scripts/native/calls/seal-w05.py")
call_gate = load_script("w05_test_gate", "scripts/native/calls/test-w05.py")


class W05GateIntegrationTest(unittest.TestCase):
    def test_coverage_report_is_exact_timestamp_free_projection(self) -> None:
        expected = validator.stable_bytes(validator.report_document())
        self.assertEqual(validator.REPORT.read_bytes(), expected)
        report = json.loads(expected)
        self.assertNotIn("timestamp", report)
        self.assertEqual(report["format_version"], "2.0.0")
        self.assertFalse(report["architecture"]["mir_executed"])
        self.assertFalse(report["architecture"]["codegen_eligible"])
        self.assertEqual(report["profile"]["unresolved_w05_count"], 0)

    def test_goldens_are_raw_complete_pipeline_output(self) -> None:
        index = json.loads(validator.GOLDEN_INDEX.read_text(encoding="utf-8"))
        self.assertFalse(index["normalization"])
        for entry in index["cases"].values():
            body = (ROOT / entry["path"]).read_text(encoding="utf-8")
            self.assertTrue(body.endswith("end\n"))
            self.assertIn("opcode call_direct_user", body)
            self.assertIn("modeled true codegen-eligible false", body)

    def test_review_manifest_binds_two_exact_v2_reviews(self) -> None:
        document = reviews.build_document()
        self.assertEqual(document["contract_commit"], reviews.CONTRACT_COMMIT)
        self.assertEqual(document["wave_pin_commit"], reviews.WAVE_PIN_COMMIT)
        self.assertEqual(
            {item["review_kind"] for item in document["reviews"]},
            {"semantics", "evidence-history-capability"},
        )
        self.assertEqual(
            len({item["sha256"] for item in document["reviews"]}), 2
        )

    def test_review_manifest_rejects_wrong_subject(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "review.json"
            document = json.loads(reviews.SEMANTICS.read_text(encoding="utf-8"))
            document["subject_commit"] = "0" * 40
            path.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaises(reviews.ReviewManifestError):
                reviews.validate_review(
                    path, "semantics", reviews.IMPLEMENTATION_HEAD
                )

    def test_workflow_uses_current_checkout_and_unique_logs(self) -> None:
        workflow = (
            ROOT / ".github/workflows/native-w05.yml"
        ).read_text(encoding="utf-8")
        self.assertNotRegex(workflow, r"git fetch .*w05")
        self.assertNotRegex(workflow, r"checkout .*-[A-Z]")
        self.assertIn("github.sha", workflow)
        log_names = re.findall(
            r'>"\$W05_ARTIFACT_ROOT/([^"]+\.log)"', workflow
        )
        self.assertGreater(len(log_names), 10)
        self.assertEqual(len(log_names), len(set(log_names)))

    def test_seal_tool_is_read_only_without_explicit_mode(self) -> None:
        completed = subprocess.run(
            [sys.executable, "scripts/native/calls/seal-w05.py"],
            cwd=ROOT,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("required", completed.stdout)

    def test_final_fault_matrix_covers_each_verifier_facet(self) -> None:
        required = {
            "structural_verifier_failure",
            "scalar_verifier_failure",
            "control_flow_verifier_failure",
            "call_verifier_failure",
            "fingerprint_recompute_failure",
        }
        self.assertTrue(required.issubset(call_gate.FAULTS))
        self.assertTrue(
            all(call_gate.FAULTS[name][2] == "MIRL0029" for name in required)
        )


if __name__ == "__main__":
    unittest.main()
