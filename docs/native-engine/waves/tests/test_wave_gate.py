import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


sys.dont_write_bytecode = True


ROOT = Path(__file__).resolve().parents[4]
SCRIPT = ROOT / "scripts/native/wave-gate.py"
DEFINITION_PATH = ROOT / "docs/native-engine/waves/waves.json"
FIXTURES = ROOT / "docs/native-engine/waves/fixtures"

SPEC = importlib.util.spec_from_file_location("wave_gate", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
wave_gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(wave_gate)


class WaveGateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.definition = wave_gate.load_definition(DEFINITION_PATH)
        cls.waves, cls.tasks = wave_gate.definition_indexes(cls.definition)
        cls.template = wave_gate.load_result(FIXTURES / "valid/task-result-D.json", cls.definition)

    def make_result(self, task_id, status="pass"):
        result = copy.deepcopy(self.template)
        result["task_id"] = task_id
        result["status"] = status
        result["branch"] = "fixture/generated-" + task_id.lower()
        result["head_commit"] = "f" * 40
        result["changed_paths"] = ["docs/native-engine/waves/generated-fixture.txt"]
        result["gate_evidence"][0]["gate_id"] = task_id
        result["gate_evidence"][0]["status"] = "pass"
        result["gate_evidence"][0]["summary"] = "Synthetic passing evidence for %s." % task_id
        result["blockers"] = []
        return result

    def write_result(self, results_dir, result):
        destination = results_dir / "W00" / (result["task_id"] + ".json")
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    def write_complete_wave(self, results_dir):
        for task_id in self.waves["W00"]["required_gate_ids"]:
            self.write_result(results_dir, self.make_result(task_id))

    def write_wave_result(self, results_dir, wave_id, result):
        destination = results_dir / wave_id / (result["task_id"] + ".json")
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    def test_definition_has_all_waves_and_stable_w00_tasks(self):
        self.assertEqual([], wave_gate.validate_definition(self.definition))
        self.assertEqual(["W%02d" % number for number in range(19)], [wave["wave_id"] for wave in self.definition["waves"]])
        expected = {
            "W00-A-repository-contracts", "W00-B-build-debug-nts", "W00-B-build-release-nts",
            "W00-B-build-debug-zts", "W00-B-build-release-zts", "W00-B-sanitizer-asan-nts",
            "W00-B-sanitizer-ubsan-nts", "W00-B-worktree-isolation", "W00-C-differential-selftest",
            "W00-C-warm-call-baseline", "W00-C-reference-manifest", "W00-D-schema-selftest",
            "W00-D-dashboard-determinism", "W00-integration-existing-tests",
            "W00-integration-no-functional-change",
        }
        self.assertEqual(expected, set(self.waves["W00"]["required_gate_ids"]))

    def test_w01_has_all_semantic_contract_gates(self):
        expected = [
            "W01-0-contract-freeze",
            "W01-A-opcode-coverage",
            "W01-B-effect-ownership-model",
            "W01-C-frame-safepoint-contract",
            "W01-D-tpde-gap-analysis",
            "W01-E-semantics-test-corpus",
            "W01-integration-gate",
        ]
        self.assertEqual(expected, self.waves["W01"]["required_gate_ids"])
        self.assertEqual(expected, [task["task_id"] for task in self.waves["W01"]["tasks"]])
        self.assertEqual("dc6e34b56846c38dc2475d6c962c2b9b7ada6df4", self.waves["W01"]["expected_base_commit"])

    def test_w01_cannot_pass_with_missing_specialist_result(self):
        with tempfile.TemporaryDirectory() as temporary:
            results_dir = Path(temporary)
            for task_id in self.waves["W01"]["required_gate_ids"]:
                if task_id != "W01-E-semantics-test-corpus":
                    result = self.make_result(task_id)
                    result["expected_base_commit"] = self.waves["W01"]["expected_base_commit"]
                    result["actual_base_commit"] = self.waves["W01"]["expected_base_commit"]
                    result["gate_evidence"][0]["wave_id"] = "W01"
                    self.write_wave_result(results_dir, "W01", result)
            summary = wave_gate.aggregate_wave(
                self.waves["W01"],
                wave_gate.load_wave_results(results_dir, "W01", self.definition),
            )
            self.assertEqual("missing", summary["status"])
            self.assertEqual(["W01-E-semantics-test-corpus"], summary["missing_gate_ids"])

    def test_w02_has_pinned_base_and_all_specialist_gates(self):
        expected = [
            "W02-A-core-arena-ids",
            "W02-B-cfg-phi-dominance",
            "W02-C-effects-ownership-binding",
            "W02-D-frame-state-source-map",
            "W02-E-text-dump-parser",
            "W02-F-verifier-stage1",
            "W02-integration-gate",
        ]
        self.assertEqual(expected, self.waves["W02"]["required_gate_ids"])
        self.assertEqual(expected, [task["task_id"] for task in self.waves["W02"]["tasks"]])
        self.assertEqual(
            "b2d0e87766fce3659a3de41ff72f06655d896aae",
            self.waves["W02"]["expected_base_commit"],
        )

    def test_w02_specialist_owned_paths_are_disjoint(self):
        specialist_tasks = self.waves["W02"]["tasks"][:6]
        owners = {}
        for task in specialist_tasks:
            for path in task["owned_paths"]:
                self.assertNotIn(path, owners, "%s is also owned by %s" % (path, owners.get(path)))
                owners[path] = task["task_id"]

    def test_w02_cannot_pass_with_missing_specialist_result(self):
        with tempfile.TemporaryDirectory() as temporary:
            results_dir = Path(temporary)
            for task_id in self.waves["W02"]["required_gate_ids"]:
                if task_id != "W02-F-verifier-stage1":
                    result = self.make_result(task_id)
                    result["expected_base_commit"] = self.waves["W02"]["expected_base_commit"]
                    result["actual_base_commit"] = self.waves["W02"]["expected_base_commit"]
                    result["gate_evidence"][0]["wave_id"] = "W02"
                    self.write_wave_result(results_dir, "W02", result)
            summary = wave_gate.aggregate_wave(
                self.waves["W02"],
                wave_gate.load_wave_results(results_dir, "W02", self.definition),
            )
            self.assertEqual("missing", summary["status"])
            self.assertEqual(["W02-F-verifier-stage1"], summary["missing_gate_ids"])

    def test_valid_task_fixtures(self):
        for name in ("A", "B", "C", "D"):
            result = json.loads((FIXTURES / ("valid/task-result-%s.json" % name)).read_text(encoding="utf-8"))
            self.assertEqual([], wave_gate.validate_result(result, self.definition), name)

    def test_valid_and_invalid_evidence_fixtures(self):
        valid = json.loads((FIXTURES / "valid/gate-evidence.json").read_text(encoding="utf-8"))
        invalid = json.loads((FIXTURES / "invalid/gate-evidence-absolute-local-path.json").read_text(encoding="utf-8"))
        self.assertEqual([], wave_gate.validate_gate_evidence(valid))
        self.assertTrue(any("absolute" in issue for issue in wave_gate.validate_gate_evidence(invalid)))

    def test_invalid_enum_missing_field_and_unknown_task(self):
        for name in ("task-result-invalid-enum.json", "task-result-missing-field.json", "task-result-unknown-task.json"):
            result = json.loads((FIXTURES / "invalid" / name).read_text(encoding="utf-8"))
            self.assertNotEqual([], wave_gate.validate_result(result, self.definition), name)

    def test_help_and_usage_exit_codes(self):
        help_run = subprocess.run([sys.executable, str(SCRIPT), "--help"], cwd=ROOT, text=True, capture_output=True, check=False)
        self.assertEqual(0, help_run.returncode)
        usage_run = subprocess.run([sys.executable, str(SCRIPT), "check"], cwd=ROOT, text=True, capture_output=True, check=False)
        self.assertEqual(wave_gate.EXIT_USAGE, usage_run.returncode)

    def test_missing_wave(self):
        with tempfile.TemporaryDirectory() as temporary:
            results = wave_gate.load_wave_results(Path(temporary), "W00", self.definition)
            summary = wave_gate.aggregate_wave(self.waves["W00"], results)
            self.assertEqual("missing", summary["status"])
            self.assertEqual(self.waves["W00"]["required_gate_ids"], summary["missing_gate_ids"])

    def test_complete_wave_passes(self):
        with tempfile.TemporaryDirectory() as temporary:
            results_dir = Path(temporary)
            self.write_complete_wave(results_dir)
            results = wave_gate.load_wave_results(results_dir, "W00", self.definition)
            summary = wave_gate.aggregate_wave(self.waves["W00"], results)
            self.assertEqual("pass", summary["status"])
            self.assertEqual([], summary["reasons"])
            self.assertEqual([], wave_gate.validate_summary(summary, self.definition))

    def test_fail_blocked_skipped_and_pending_states(self):
        expected = {"fail": "fail", "blocked": "blocked", "skipped": "fail", "pending": "pending", "running": "running"}
        for task_status, wave_status in expected.items():
            with self.subTest(task_status=task_status), tempfile.TemporaryDirectory() as temporary:
                results_dir = Path(temporary)
                self.write_complete_wave(results_dir)
                changed = self.make_result("W00-D-schema-selftest", task_status)
                self.write_result(results_dir, changed)
                summary = wave_gate.aggregate_wave(self.waves["W00"], wave_gate.load_wave_results(results_dir, "W00", self.definition))
                self.assertEqual(wave_status, summary["status"])

    def test_stale_base_dirty_worktree_missing_evidence_and_failed_test_never_pass(self):
        mutations = (
            lambda result: result.update(expected_base_commit="1" * 40, actual_base_commit="1" * 40),
            lambda result: result.update(worktree_clean=False),
            lambda result: result.update(gate_evidence=[]),
            lambda result: result["tests"][0].update(status="fail", exit_code=1),
            lambda result: result.update(tests=[]),
            lambda result: result.update(acceptance_criteria=[]),
            lambda result: result.update(head_commit=None),
            lambda result: result["gate_evidence"].append(dict(result["gate_evidence"][0], status="blocked")),
        )
        for mutate in mutations:
            with self.subTest(mutation=mutate), tempfile.TemporaryDirectory() as temporary:
                results_dir = Path(temporary)
                self.write_complete_wave(results_dir)
                changed = self.make_result("W00-D-schema-selftest")
                mutate(changed)
                self.write_result(results_dir, changed)
                summary = wave_gate.aggregate_wave(self.waves["W00"], wave_gate.load_wave_results(results_dir, "W00", self.definition))
                self.assertEqual("fail", summary["status"])

    def test_record_is_idempotent_and_duplicate_requires_replace_with_audit(self):
        with tempfile.TemporaryDirectory() as temporary:
            results_dir = Path(temporary)
            source = FIXTURES / "valid/task-result-D.json"
            args = type("Args", (), {"definition": DEFINITION_PATH, "result": source, "results_dir": results_dir, "wave": "W00", "replace": False})()
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(0, wave_gate.command_record(args))
                self.assertEqual(0, wave_gate.command_record(args))
            modified = json.loads(source.read_text(encoding="utf-8"))
            modified["head_commit"] = "9" * 40
            modified_path = results_dir / "modified.json"
            wave_gate.write_json_atomic(modified_path, modified)
            args.result = modified_path
            with self.assertRaises(wave_gate.ValidationError):
                wave_gate.command_record(args)
            args.replace = True
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(0, wave_gate.command_record(args))
            audit = json.loads((results_dir / ".audit.json").read_text(encoding="utf-8"))
            self.assertEqual(1, len(audit["replacements"]))
            self.assertEqual("d" * 40, audit["replacements"][0]["old_identity"]["head_commit"])
            self.assertEqual("9" * 40, audit["replacements"][0]["new_identity"]["head_commit"])

    def test_atomic_writer_cleans_temporary_file_on_replace_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "result.json"
            with mock.patch.object(wave_gate.os, "replace", side_effect=OSError("synthetic rename failure")):
                with self.assertRaises(OSError):
                    wave_gate.write_json_atomic(destination, {"value": 1})
            self.assertFalse(destination.exists())
            self.assertEqual([], list(Path(temporary).iterdir()))

    def test_render_is_byte_deterministic_and_timestamps_are_opt_in(self):
        with tempfile.TemporaryDirectory() as temporary:
            results_dir = Path(temporary) / "results"
            self.write_result(results_dir, self.make_result("W00-D-schema-selftest"))
            first = wave_gate.render_dashboard(self.definition, results_dir, False)
            second = wave_gate.render_dashboard(self.definition, results_dir, False)
            self.assertEqual(first.encode("utf-8"), second.encode("utf-8"))
            self.assertNotIn(self.template["timestamp"], first)
            with_timestamps = wave_gate.render_dashboard(self.definition, results_dir, True)
            self.assertIn(self.template["timestamp"], with_timestamps)

    def test_render_sorts_waves_and_explains_semantic_failures(self):
        with tempfile.TemporaryDirectory() as temporary:
            results_dir = Path(temporary)
            self.write_complete_wave(results_dir)
            stale = self.make_result("W00-D-schema-selftest")
            stale["expected_base_commit"] = "1" * 40
            stale["actual_base_commit"] = "1" * 40
            self.write_result(results_dir, stale)
            reversed_definition = copy.deepcopy(self.definition)
            reversed_definition["waves"].reverse()
            dashboard = wave_gate.render_dashboard(reversed_definition, results_dir, False)
            self.assertLess(dashboard.index("## W00"), dashboard.index("## W01"))
            self.assertIn("declares stale expected base", dashboard)

    def test_list_missing_cli_and_check_cli(self):
        with tempfile.TemporaryDirectory() as temporary:
            missing = subprocess.run(
                [sys.executable, str(SCRIPT), "list-missing", "--wave", "W00", "--results-dir", temporary],
                cwd=ROOT, text=True, capture_output=True, check=False,
            )
            self.assertEqual(wave_gate.EXIT_GATE_FAIL, missing.returncode)
            self.assertIn("W00-A-repository-contracts", missing.stdout)
            check = subprocess.run(
                [sys.executable, str(SCRIPT), "check", "--wave", "W00", "--results-dir", temporary],
                cwd=ROOT, text=True, capture_output=True, check=False,
            )
            self.assertEqual(wave_gate.EXIT_GATE_FAIL, check.returncode)
            self.assertEqual("missing", json.loads(check.stdout)["status"])

    def test_check_cli_pass_fail_and_blocked(self):
        for task_status, expected_status, expected_exit in (
            ("pass", "pass", wave_gate.EXIT_OK),
            ("fail", "fail", wave_gate.EXIT_GATE_FAIL),
            ("blocked", "blocked", wave_gate.EXIT_GATE_FAIL),
        ):
            with self.subTest(task_status=task_status), tempfile.TemporaryDirectory() as temporary:
                results_dir = Path(temporary)
                self.write_complete_wave(results_dir)
                if task_status != "pass":
                    self.write_result(results_dir, self.make_result("W00-D-schema-selftest", task_status))
                check = subprocess.run(
                    [sys.executable, str(SCRIPT), "check", "--wave", "W00", "--results-dir", temporary],
                    cwd=ROOT, text=True, capture_output=True, check=False,
                )
                self.assertEqual(expected_exit, check.returncode)
                self.assertEqual(expected_status, json.loads(check.stdout)["status"])


if __name__ == "__main__":
    unittest.main()
