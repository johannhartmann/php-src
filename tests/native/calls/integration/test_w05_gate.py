"""Integration-level checks for the W05 gate and durable-seal tooling."""

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


class W05GateIntegrationTest(unittest.TestCase):
    def test_coverage_report_is_exact_timestamp_free_projection(self) -> None:
        expected = validator.stable_bytes(validator.report_document())
        self.assertEqual(validator.REPORT.read_bytes(), expected)
        report = json.loads(expected)
        self.assertNotIn("timestamp", report)
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

    def test_review_manifest_rejects_wrong_head_and_empty_review(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            markdown = root / "review.md"
            payload = root / "review.json"
            markdown.write_text("approved\n", encoding="utf-8")
            payload.write_text(
                json.dumps(
                    {
                        "approved": True,
                        "findings": [],
                        "reviewed_head": "0" * 40,
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaises(reviews.ReviewManifestError):
                reviews.review_entry(
                    "W05-R1", payload, markdown, "1" * 40
                )
            markdown.write_bytes(b"")
            with self.assertRaises(reviews.ReviewManifestError):
                reviews.review_entry(
                    "W05-R1", payload, markdown, "0" * 40
                )

    def test_review_manifest_accepts_prescribed_commit_fields(self) -> None:
        head = "a" * 40
        self.assertEqual(reviews.reviewed_head({"commit": head}), head)
        self.assertEqual(
            reviews.reviewed_head(
                {"commit_chain": {"implementation_head_m": head}}
            ),
            head,
        )
        self.assertIsNone(
            reviews.reviewed_head(
                {"commit": head, "reviewed_head": "b" * 40}
            )
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

    def test_seal_tool_is_read_only_without_write(self) -> None:
        completed = subprocess.run(
            [
                sys.executable,
                "scripts/native/calls/seal-w05.py",
                "--subject",
                "0" * 40,
            ],
            cwd=ROOT,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("--write", completed.stdout)

    def test_seal_binds_every_w05_build_profile(self) -> None:
        commands = dict(seal.COMMANDS)
        command_ids = set(commands)
        self.assertTrue(
            {
                "build-w05-debug-nts",
                "build-w05-debug-zts",
                "build-w05-asan-nts",
                "build-w05-ubsan-nts",
            }.issubset(command_ids)
        )
        self.assertIn(
            "TEST_PHP_EXECUTABLE={candidate}",
            commands["calls-unittest"],
        )
        self.assertIn("--candidate-php {candidate}", commands["fuzz-20000"])


if __name__ == "__main__":
    unittest.main()
