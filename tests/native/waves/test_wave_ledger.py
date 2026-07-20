import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
WAVE_GATE_PATH = ROOT / "scripts/native/wave-gate.py"
DEFINITION_PATH = ROOT / "docs/native-engine/waves/waves.json"
LEDGER_PATH = ROOT / "docs/native-engine/waves/ledger.json"

SPEC = importlib.util.spec_from_file_location("w05_wave_gate", WAVE_GATE_PATH)
assert SPEC is not None and SPEC.loader is not None
wave_gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(wave_gate)


class WaveLedgerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.definition = wave_gate.load_definition(DEFINITION_PATH)
        cls.ledger = wave_gate.load_ledger(LEDGER_PATH, cls.definition)

    def test_ledger_has_exact_wave_set_and_historical_waves_are_unsealed(self):
        self.assertEqual([], wave_gate.validate_ledger(self.ledger, self.definition))
        entries = {entry["wave_id"]: entry for entry in self.ledger["waves"]}
        self.assertEqual(
            ["W%02d" % number for number in range(19)],
            sorted(entries),
        )
        for wave_id in ("W00", "W01", "W02", "W03"):
            self.assertEqual("unsealed", entries[wave_id]["state"])
            self.assertIsNone(entries[wave_id]["receipt_path"])
            self.assertIsNone(entries[wave_id]["receipt_sha256"])
        self.assertEqual("revalidated", entries["W04"]["state"])
        self.assertEqual(
            "docs/native-engine/waves/receipts/W04.json",
            entries["W04"]["receipt_path"],
        )
        self.assertEqual("sealed", entries["W05"]["state"])
        self.assertEqual(
            "docs/native-engine/waves/receipts/W05.json",
            entries["W05"]["receipt_path"],
        )
        for number in range(6, 19):
            self.assertEqual("unstarted", entries["W%02d" % number]["state"])

    def test_invalid_receipt_binding_is_rejected(self):
        ledger = copy.deepcopy(self.ledger)
        entry = ledger["waves"][0]
        entry["receipt_path"] = "docs/native-engine/waves/receipts/W00.json"
        entry["receipt_sha256"] = "0" * 64
        issues = wave_gate.validate_ledger(ledger, self.definition)
        self.assertTrue(
            any("may bind a receipt only" in issue for issue in issues),
            issues,
        )

    def test_ledger_dashboard_is_deterministic_and_has_no_timestamp(self):
        first = wave_gate.render_ledger_dashboard(self.definition, self.ledger)
        second = wave_gate.render_ledger_dashboard(self.definition, self.ledger)
        self.assertEqual(first.encode("utf-8"), second.encode("utf-8"))
        self.assertNotIn("created_at", first)
        self.assertNotIn("Timestamp", first)
        self.assertIn("**Status:** `UNSEALED`", first)
        self.assertIn("**Status:** `SEALED`", first)
        self.assertIn("**Status:** `UNSTARTED`", first)

    def test_explicit_results_commands_preserve_existing_result_mode(self):
        parser = wave_gate.build_parser()
        explicit = parser.parse_args([
            "check-results", "--wave", "W04", "--results-dir", "/tmp/results",
        ])
        legacy = parser.parse_args([
            "check", "--wave", "W04", "--results-dir", "/tmp/results",
        ])
        self.assertIs(explicit.handler, wave_gate.command_check_results)
        self.assertIs(legacy.handler, wave_gate.command_check)
        self.assertEqual(explicit.results_dir, legacy.results_dir)

    def test_default_render_reads_ledger(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "status.md"
            parser = wave_gate.build_parser()
            args = parser.parse_args(["render", "--output", str(output)])
            self.assertEqual(0, args.handler(args))
            self.assertEqual(
                wave_gate.render_ledger_dashboard(self.definition, self.ledger),
                output.read_text(encoding="utf-8"),
            )


if __name__ == "__main__":
    unittest.main()
