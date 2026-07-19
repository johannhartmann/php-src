import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts/native/verify-wave-receipt.py"

SPEC = importlib.util.spec_from_file_location("w05_verify_receipt", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
verify = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verify)


def git_bytes(specification):
    return subprocess.check_output(["git", "show", specification], cwd=ROOT)


def git_text(*arguments):
    return subprocess.check_output(
        ["git", *arguments], cwd=ROOT, text=True
    ).strip()


def sha256(value):
    return hashlib.sha256(value).hexdigest()


class WaveReceiptTests(unittest.TestCase):
    def make_fixture(self, directory):
        subject = git_text("rev-parse", "HEAD")
        tree = git_text("rev-parse", "HEAD^{tree}")
        definition_path = "docs/native-engine/waves/waves.json"
        profile_path = "docs/native-engine/control-flow/w04-opcode-profile.json"
        coverage_path = "docs/native-engine/control-flow/w04-coverage-report.json"
        artifact_root = directory / "artifacts"
        artifact_root.mkdir()
        log_path = artifact_root / "validate.log"
        log_path.write_bytes(b"command output\n")
        receipt = {
            "artifact_digests": [{
                "artifact_id": "validate-log",
                "command_id": "validate",
                "path": "validate.log",
                "sha256": sha256(log_path.read_bytes()),
                "size_bytes": log_path.stat().st_size,
            }],
            "capabilities_provided": ["reducible_control_flow"],
            "codegen_eligible": False,
            "command_manifest": [{
                "artifact_id": "validate-log",
                "command": "python3 validate.py --check",
                "command_id": "validate",
                "exit_code": 0,
            }],
            "coverage_report_path": coverage_path,
            "coverage_report_sha256": sha256(git_bytes("%s:%s" % (subject, coverage_path))),
            "created_at": "2026-07-19T12:00:00Z",
            "definition_path": definition_path,
            "definition_sha256": sha256(git_bytes("%s:%s" % (subject, definition_path))),
            "dependency_receipts": [],
            "format_version": "1.0.0",
            "profile_paths": [profile_path],
            "profile_sha256": [sha256(git_bytes("%s:%s" % (subject, profile_path)))],
            "review_digests": [],
            "semantic_debts": ["call_execution"],
            "state": "revalidated",
            "subject_commit": subject,
            "subject_tree": tree,
            "wave_id": "W04",
        }
        receipt_dir = ROOT / ".w05-receipt-test"
        receipt_dir.mkdir(exist_ok=True)
        receipt_path = receipt_dir / (directory.name + ".json")
        receipt_path.write_text(
            json.dumps(receipt, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        ledger = {
            "format_version": "1.0.0",
            "migration_policy": "fixture",
            "waves": [{
                "capabilities_provided": receipt["capabilities_provided"],
                "codegen_eligible": False,
                "receipt_path": receipt_path.relative_to(ROOT).as_posix(),
                "receipt_sha256": verify.digest_file(receipt_path),
                "semantic_debts": receipt["semantic_debts"],
                "state": "revalidated",
                "wave_id": "W04",
            }],
        }
        return receipt, ledger, receipt_path, artifact_root

    def tearDown(self):
        receipt_dir = ROOT / ".w05-receipt-test"
        if receipt_dir.exists():
            for path in receipt_dir.iterdir():
                path.unlink()
            receipt_dir.rmdir()

    def test_valid_receipt_binds_subject_tree_and_distinct_log(self):
        with tempfile.TemporaryDirectory() as temporary:
            receipt, ledger, path, root = self.make_fixture(Path(temporary))
            self.assertEqual(
                [],
                verify.verify_receipt(receipt, ledger, path, root),
            )

    def test_hash_corruption_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            receipt, ledger, path, root = self.make_fixture(Path(temporary))
            receipt["definition_sha256"] = "0" * 64
            issues = verify.verify_receipt(receipt, ledger, path, root)
            self.assertTrue(any("definition digest mismatch" in item for item in issues), issues)

    def test_wrong_commit_or_tree_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            receipt, ledger, path, root = self.make_fixture(Path(temporary))
            receipt["subject_tree"] = "0" * 40
            issues = verify.verify_receipt(receipt, ledger, path, root)
            self.assertTrue(any("subject_tree mismatch" in item for item in issues), issues)

    def test_missing_external_artifact_is_rejected_when_root_is_supplied(self):
        with tempfile.TemporaryDirectory() as temporary:
            receipt, ledger, path, root = self.make_fixture(Path(temporary))
            (root / "validate.log").unlink()
            issues = verify.verify_receipt(receipt, ledger, path, root)
            self.assertTrue(any("is missing" in item for item in issues), issues)

    def test_timestamp_does_not_participate_in_repository_or_log_digests(self):
        with tempfile.TemporaryDirectory() as temporary:
            receipt, ledger, path, root = self.make_fixture(Path(temporary))
            before = [
                receipt["definition_sha256"],
                receipt["coverage_report_sha256"],
                *receipt["profile_sha256"],
                receipt["artifact_digests"][0]["sha256"],
            ]
            receipt["created_at"] = "2030-01-01T00:00:00Z"
            after = [
                receipt["definition_sha256"],
                receipt["coverage_report_sha256"],
                *receipt["profile_sha256"],
                receipt["artifact_digests"][0]["sha256"],
            ]
            self.assertEqual(before, after)


if __name__ == "__main__":
    unittest.main()
