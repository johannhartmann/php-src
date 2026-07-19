from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[4]


def load_module(name: str, relative: str):
    path = ROOT / relative
    specification = importlib.util.spec_from_file_location(name, path)
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


validator = load_module(
    "w04_integration_validator",
    "scripts/native/control-flow/validate-w04.py",
)
recorder = load_module(
    "w04_integration_recorder",
    "scripts/native/control-flow/record-w04-results.py",
)


class W04IntegrationTests(unittest.TestCase):
    def test_config_is_generated_from_the_frozen_source_manifest(self) -> None:
        manifest = validator.load_json(validator.SOURCE_MANIFEST_PATH)
        validator.validate_config(manifest)
        self.assertEqual(
            validator.config_w04_sources(),
            sorted(manifest["w04_production_sources"]),
        )

    def test_timestamp_free_report_is_current(self) -> None:
        expected = validator.stable_bytes(validator.report_document())
        self.assertEqual(validator.REPORT_PATH.read_bytes(), expected)
        document = json.loads(expected)
        self.assertNotIn("timestamp", document)
        self.assertNotIn("generated_at", document)
        self.assertFalse(document["determinism"]["report_has_timestamp"])

    def test_specialist_provenance_uses_distinct_real_heads(self) -> None:
        report = validator.load_json(validator.REPORT_PATH)
        heads = report["ownership"]["specialist_heads"]
        self.assertEqual(heads["W04-A-production-control-flow"], validator.A_HEAD)
        self.assertEqual(heads["W04-B-control-flow-evidence"], validator.B_HEAD)
        self.assertNotEqual(validator.A_HEAD, validator.B_HEAD)
        self.assertEqual(recorder.DEFAULT_A_HEAD, validator.A_HEAD)
        self.assertEqual(recorder.DEFAULT_B_HEAD, validator.B_HEAD)

    def test_goldens_cover_branch_phi_loop_and_edge_statepoints(self) -> None:
        index = validator.load_json(validator.GOLDEN_INDEX)
        totals = {
            key: sum(entry["cfg"][key] for entry in index["cases"].values())
            for key in ("blocks", "edges", "phis", "backedges", "statepoints")
        }
        self.assertGreater(totals["blocks"], 0)
        self.assertGreater(totals["edges"], 0)
        self.assertGreater(totals["phis"], 0)
        self.assertGreater(totals["backedges"], 0)
        self.assertGreater(totals["statepoints"], 0)
        for entry in index["cases"].values():
            path = ROOT / entry["path"]
            self.assertEqual(
                hashlib.sha256(path.read_bytes()).hexdigest(),
                entry["sha256"],
            )

    def test_true_successor_precedes_false_successor(self) -> None:
        mir = (
            ROOT
            / "tests/native/control-flow/integration/goldens/if_else_int.znmir"
        ).read_text(encoding="utf-8")
        self.assertIn("block b0 function f0 predecessors [] successors [b1, b2]", mir)
        self.assertIn(
            "instruction i3 block b0 opcode cond_branch "
            "representation control result invalid operands [v1]",
            mir,
        )
        self.assertIn(
            "instruction i4 block b1 opcode return "
            "representation void result invalid operands [v2147483648]",
            mir,
        )
        self.assertIn(
            "instruction i5 block b2 opcode return "
            "representation void result invalid operands [v2147483649]",
            mir,
        )

    def test_interrupt_edge_has_closed_w01_semantics(self) -> None:
        mir = (
            ROOT
            / "tests/native/control-flow/integration/goldens/while_loop_counter.znmir"
        ).read_text(encoding="utf-8")
        statepoint = next(
            line
            for line in mir.splitlines()
            if "opcode statepoint" in line and "effects 0x0400" in line
        )
        self.assertIn("reads 0x00004008", statepoint)
        self.assertIn("writes 0x00004000", statepoint)
        self.assertIn("barriers 0x61", statepoint)

    def test_report_has_no_runtime_activation(self) -> None:
        report = validator.load_json(validator.REPORT_PATH)
        architecture = report["architecture_independence"]
        self.assertEqual(architecture["leak_count"], 0)
        self.assertFalse(architecture["product_runtime_activation"])
        self.assertTrue(architecture["test_extension_default_off"])

    def test_ci_restores_stub_generator_output_before_clean_gate(self) -> None:
        workflow = (ROOT / ".github/workflows/native-w04.yml").read_text(
            encoding="utf-8"
        )
        restore = (
            "git restore --worktree "
            "ext/native_mir_test/native_mir_test_arginfo.h"
        )
        self.assertIn(restore, workflow)
        self.assertLess(workflow.index(restore), workflow.index("git diff --exit-code"))


def load_tests(
    loader: unittest.TestLoader,
    standard_tests: unittest.TestSuite,
    pattern: str | None,
) -> unittest.TestSuite:
    """Include the non-package W04 suites in root-level discovery."""
    suite = unittest.TestSuite()
    suite.addTests(standard_tests)
    for suite_name in ("bridge", "contracts", "unit"):
        suite_dir = ROOT / "tests" / "native" / "control-flow" / suite_name
        for test_path in sorted(suite_dir.glob("test_*.py")):
            module = load_module(
                f"_w04_{suite_name}_{test_path.stem}",
                str(test_path.relative_to(ROOT)),
            )
            suite.addTests(loader.loadTestsFromModule(module))
    return suite


if __name__ == "__main__":
    unittest.main()
