from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[3]
VERIFY_PATH = ROOT / "scripts/native/verify-wave-receipt.py"
HISTORY_PATH = ROOT / "scripts/native/check-phase-history.py"
WAVE_GATE_PATH = ROOT / "scripts/native/wave-gate.py"


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


VERIFY = load("w05_v2_verify", VERIFY_PATH)
HISTORY = load("w05_v2_history", HISTORY_PATH)
WAVE_GATE = load("w05_v2_wave_gate", WAVE_GATE_PATH)


def phase_receipt(phase_id: str) -> dict[str, object]:
    return {
        "format_version": "1.0.0",
        "phase_id": phase_id,
        "commit": "1" * 40,
        "tree": "2" * 40,
        "parent": "3" * 40,
        "changed_paths": ["path/file.c"],
        "command_summary_digests": [
            {
                "command_id": "contract-check",
                "path": "summaries/contract-check.json",
                "sha256": "4" * 64,
            }
        ],
    }


class W05V2ContractTests(unittest.TestCase):
    def test_registry_ids_are_unique_and_references_exist(self) -> None:
        registry = json.loads(
            (ROOT / "docs/native-engine/roadmap/capability-registry.json").read_text()
        )
        entries = registry["entries"]
        ids = {(entry["kind"], entry["id"]) for entry in entries}
        referenced_ids = {entry["id"] for entry in entries}
        transports = {entry["transport_id"] for entry in entries}
        self.assertEqual(len(entries), len(ids))
        self.assertEqual(len(entries), len(transports))
        for entry in entries:
            self.assertLessEqual(
                set(entry["prerequisites"] + entry["closes_debts"] + entry["incompatible_with"]),
                referenced_ids,
            )

    def test_v2_shape_rejects_missing_reviews(self) -> None:
        fixture = {
            "format_version": "2.0.0",
            "wave_id": "W05",
            "state": "sealed",
            "verification_level": "full",
            "subject_commit": "0" * 40,
            "subject_tree": "0" * 40,
            "definition": {},
            "profiles": [],
            "coverage": {},
            "command_summaries": [],
            "binary_manifests": [],
            "dependency_receipts": [],
            "phase_receipts": [],
            "capability_ids": [],
            "semantic_debt_ids": [],
            "codegen_eligible": False,
        }
        self.assertTrue(
            any(
                "reviews" in issue
                for issue in VERIFY.validate_v2_shape(fixture)
            )
        )

    def test_v2_shape_requires_evidence_minimums(self) -> None:
        fixture = {
            "format_version": "2.0.0",
            "wave_id": "W05",
            "state": "sealed",
            "verification_level": "full",
            "subject_commit": "0" * 40,
            "subject_tree": "0" * 40,
            "definition": {"path": "definition.json", "sha256": "0" * 64},
            "profiles": [],
            "coverage": {"path": "coverage.json", "sha256": "0" * 64},
            "command_summaries": [],
            "binary_manifests": [],
            "reviews": [],
            "dependency_receipts": [],
            "phase_receipts": [],
            "capability_ids": [],
            "semantic_debt_ids": [],
            "codegen_eligible": False,
        }
        issues = VERIFY.validate_v2_shape(fixture)
        for field in (
            "command_summaries", "binary_manifests", "reviews",
            "phase_receipts",
        ):
            self.assertTrue(any(field in issue for issue in issues), issues)

    def test_v2_shape_rejects_invalid_wave_and_unknown_field(self) -> None:
        fixture = {
            "format_version": "2.0.0",
            "wave_id": "W99",
            "state": "sealed",
            "verification_level": "full",
            "subject_commit": "0" * 40,
            "subject_tree": "0" * 40,
            "definition": {"path": "definition.json", "sha256": "0" * 64},
            "profiles": [],
            "coverage": {"path": "coverage.json", "sha256": "0" * 64},
            "command_summaries": [],
            "binary_manifests": [],
            "reviews": [],
            "dependency_receipts": [],
            "phase_receipts": [],
            "capability_ids": [],
            "semantic_debt_ids": [],
            "codegen_eligible": False,
            "unexpected": True,
        }
        issues = VERIFY.validate_v2_shape(fixture)
        self.assertTrue(any("wave_id" in issue for issue in issues), issues)
        self.assertTrue(any("unknown fields" in issue for issue in issues), issues)

    def test_embedded_phase_receipt_schemas_match_standalone(self) -> None:
        standalone = json.loads(
            (
                ROOT
                / "docs/native-engine/waves/schemas/phase-receipt.schema.json"
            ).read_text()
        )
        wave = json.loads(
            (
                ROOT
                / "docs/native-engine/waves/schemas/"
                "wave-receipt-v2.schema.json"
            ).read_text()
        )
        task = json.loads(
            (
                ROOT
                / "docs/native-engine/waves/schemas/"
                "codex-task-result.schema.json"
            ).read_text()
        )
        embedded = wave["properties"]["phase_receipts"]["items"]
        task_embedded = task["$defs"]["phaseReceipt"]
        self.assertEqual(standalone["required"], embedded["required"])
        self.assertEqual(standalone["required"], task_embedded["required"])
        expected_summary = standalone["properties"][
            "command_summary_digests"
        ]["items"]["required"]
        self.assertEqual(
            expected_summary,
            embedded["properties"]["command_summary_digests"][
                "items"
            ]["required"],
        )
        self.assertEqual(
            expected_summary,
            task_embedded["properties"]["command_summary_digests"][
                "items"
            ]["required"],
        )

    def test_phase_receipts_allow_multiple_contiguous_repairs(self) -> None:
        receipts = [
            phase_receipt("W05-v2-contract"),
            phase_receipt("W05-v2-wave-pin"),
            phase_receipt("W05-v2-implementation"),
            phase_receipt("W05-v2-implementation"),
            phase_receipt("W05-v2-gate"),
        ]
        issues: list[str] = []
        WAVE_GATE.validate_phase_receipts(
            receipts, "$.phase_receipts", issues
        )
        self.assertEqual([], issues)

    def test_phase_receipt_rejects_missing_format_and_command_id(self) -> None:
        receipt = phase_receipt("W05-v2-contract")
        del receipt["format_version"]
        del receipt["command_summary_digests"][0]["command_id"]
        issues: list[str] = []
        WAVE_GATE.validate_phase_receipts(
            [receipt], "$.phase_receipts", issues
        )
        self.assertTrue(any("format_version" in issue for issue in issues))
        self.assertTrue(any("command_id" in issue for issue in issues))

    def test_w05_task_result_requires_phase_receipts(self) -> None:
        definition = json.loads(
            (ROOT / "docs/native-engine/waves/waves.json").read_text()
        )
        result = {
            "format_version": "1.0.0",
            "task_id": "W05-integration-gate",
            "status": "pass",
            "expected_base_commit": "1" * 40,
            "actual_base_commit": "1" * 40,
            "head_commit": None,
            "tested_head_commit": "2" * 40,
            "seal_subject": {
                "receipt_path": "receipts/W05.json",
                "receipt_sha256": "3" * 64,
            },
            "branch": "integration/wave-06",
            "changed_paths": [],
            "tests": [],
            "acceptance_criteria": [],
            "gate_evidence": [],
            "risks": [],
            "blockers": [],
            "worktree_clean": True,
            "timestamp": "2026-07-20T00:00:00Z",
        }
        issues = WAVE_GATE.validate_result(result, definition)
        self.assertTrue(any("phase_receipts" in issue for issue in issues))

    def test_phase_and_nested_task_fields_are_strict(self) -> None:
        receipts = [
            phase_receipt("W05-v2-contract"),
            phase_receipt("W05-v2-wave-pin"),
            phase_receipt("W05-v2-implementation"),
            phase_receipt("W05-v2-gate"),
        ]
        receipts[0]["changed_paths"] = ["../escape"]
        issues: list[str] = []
        WAVE_GATE.validate_phase_receipts(
            receipts, "$.phase_receipts", issues
        )
        self.assertTrue(any("traverse parents" in issue for issue in issues))

        definition = json.loads(
            (ROOT / "docs/native-engine/waves/waves.json").read_text()
        )
        result = {
            "format_version": "1.0.0",
            "task_id": "W05-integration-gate",
            "status": "pass",
            "expected_base_commit": "1" * 40,
            "actual_base_commit": "1" * 40,
            "head_commit": None,
            "tested_head_commit": "2" * 40,
            "seal_subject": {
                "receipt_path": "receipts/W05.json",
                "receipt_sha256": "3" * 64,
            },
            "phase_receipts": receipts,
            "branch": "integration/wave-06",
            "changed_paths": [],
            "tests": [{
                "command": "true",
                "status": "pass",
                "exit_code": 0,
                "duration_ms": 0,
                "artifact": {"kind": "local", "reference": "true.log"},
                "unexpected": True,
            }],
            "acceptance_criteria": [],
            "gate_evidence": [],
            "risks": [],
            "blockers": [],
            "worktree_clean": True,
            "timestamp": "2026-07-20T00:00:00Z",
        }
        issues = WAVE_GATE.validate_result(result, definition)
        self.assertTrue(any("unknown fields" in issue for issue in issues))

    def test_command_summary_rejects_unknown_and_boolean_integers(self) -> None:
        summary = {
            "format_version": "2.0.0",
            "command_id": "contract-check",
            "argv": ["python3", "check.py"],
            "environment_profile": "contract",
            "exit_code": True,
            "stdout_sha256": "1" * 64,
            "stderr_sha256": "2" * 64,
            "duration_ms": False,
            "unexpected": True,
        }
        issues: list[str] = []
        VERIFY.validate_command_summary(summary, "summary", issues)
        self.assertTrue(any("field set" in issue for issue in issues), issues)
        self.assertTrue(any("integer exit code" in issue for issue in issues), issues)
        self.assertTrue(any("duration_ms" in issue for issue in issues), issues)

    def test_review_content_rejects_unknown_kind_and_empty_reviewer(self) -> None:
        review = {
            "format_version": "2.0.0",
            "review_id": "W05-R1",
            "review_kind": "bogus",
            "subject_commit": "1" * 40,
            "subject_tree": "2" * 40,
            "reviewer": "",
            "status": "pass",
            "findings": [],
        }
        issues: list[str] = []
        with mock.patch.object(VERIFY, "git_output", return_value="2" * 40):
            VERIFY.validate_review(
                json.dumps(review).encode(), "1" * 40, "review", issues
            )
        self.assertTrue(any("review_kind" in issue for issue in issues), issues)
        self.assertTrue(any("reviewer" in issue for issue in issues), issues)

    def test_binary_manifest_rejects_absolute_configure_argument(self) -> None:
        manifest = {
            "binary_id": "candidate",
            "role": "candidate",
            "artifact_path": "binaries/candidate-php",
            "sha256": "0" * 64,
            "git_commit": "1" * 40,
            "git_tree": "2" * 40,
            "configure_args": ["--prefix=/tmp/private"],
            "toolchain": "clang",
        }
        issues: list[str] = []
        VERIFY.validate_binary_manifests([manifest], issues)
        self.assertTrue(any("configure_args" in issue for issue in issues), issues)

    def test_candidate_manifest_is_bound_to_receipt_subject(self) -> None:
        manifests = []
        for role in ("reference", "candidate"):
            manifests.append(
                {
                    "binary_id": role,
                    "role": role,
                    "artifact_path": "binaries/%s-php" % role,
                    "sha256": "0" * 64,
                    "git_commit": "1" * 40,
                    "git_tree": "2" * 40,
                    "configure_args": ["--disable-all"],
                    "toolchain": "clang",
                }
            )
        issues: list[str] = []
        VERIFY.validate_binary_manifests(
            manifests, issues, "3" * 40, "4" * 40
        )
        self.assertTrue(
            any("candidate commit" in issue for issue in issues), issues
        )
        self.assertTrue(
            any("candidate tree" in issue for issue in issues), issues
        )

    def test_reviews_bind_to_last_implementation_receipt(self) -> None:
        source = VERIFY_PATH.read_text(encoding="utf-8")
        self.assertIn("implementation_commits[-1]", source)

    def test_dependency_receipts_are_read_from_subject_commit(self) -> None:
        source = VERIFY_PATH.read_text(encoding="utf-8")
        self.assertIn(
            '"show", "%s:%s" % (subject, path), binary=True', source
        )

    def test_command_summary_reference_binds_command_id(self) -> None:
        issues: list[str] = []
        VERIFY.validate_command_summary_reference(
            {"command_id": "check-contract", "path": "summary.json",
             "sha256": "0" * 64},
            "summary",
            issues,
        )
        self.assertEqual([], issues)

    def test_absolute_command_token_is_rejected(self) -> None:
        self.assertFalse(VERIFY.is_relative_path("/tmp/php"))
        self.assertFalse(VERIFY.is_relative_path("../result.json"))
        self.assertTrue(VERIFY.is_relative_path("artifacts/result.json"))

    def test_commitwise_history_observes_modify_then_revert(self) -> None:
        source = HISTORY_PATH.read_text(encoding="utf-8")
        self.assertIn('"diff-tree"', source)
        self.assertNotIn('"diff", "--name-only", f"{start}..HEAD"', source)

    def test_history_parent_count_detects_merge(self) -> None:
        with mock.patch.object(
            HISTORY, "git", return_value="commit parent-one parent-two"
        ):
            self.assertEqual(2, HISTORY.parent_count("commit"))

    def test_parameter_mode_boundaries_are_frozen(self) -> None:
        fixtures = json.loads(
            (ROOT / "tests/native/calls/contracts/call-sequence-fixtures.json").read_text()
        )
        case = next(
            item for item in fixtures["cases"]
            if item["name"] == "parameter-mode-boundaries"
        )
        self.assertEqual([0, 63, 64, 127], case["ordinals"])


if __name__ == "__main__":
    unittest.main()
