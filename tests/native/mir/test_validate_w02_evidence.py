"""Regression tests for the historical W02 coverage-evidence model."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[3]
VALIDATOR_PATH = ROOT / "scripts/native/mir/validate-w02.py"
SPEC = importlib.util.spec_from_file_location("validate_w02_evidence", VALIDATOR_PATH)
assert SPEC is not None and SPEC.loader is not None
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)


class W02HistoricalEvidenceTests(unittest.TestCase):
    def test_report_binds_a_committed_subject_and_tree(self) -> None:
        report = json.loads(VALIDATOR.REPORT.read_text(encoding="utf-8"))
        subject = VALIDATOR.report_subject(report)
        self.assertEqual(
            VALIDATOR.subject_tree(subject),
            report["evidence"]["subject_tree"],
        )
        self.assertEqual("historical-source-snapshot", report["evidence"]["scope"])

    def test_report_is_deterministic_for_its_bound_subject(self) -> None:
        report = json.loads(VALIDATOR.REPORT.read_text(encoding="utf-8"))
        subject = VALIDATOR.report_subject(report)
        self.assertEqual(VALIDATOR.REPORT.read_bytes(), VALIDATOR.report_bytes(subject))

    def test_live_inventory_growth_cannot_repin_historical_evidence(self) -> None:
        expected = VALIDATOR.report_bytes()
        with (
            mock.patch.object(
                VALIDATOR,
                "production_sources",
                return_value=(Path("/future/wave/source.c"),),
            ),
            mock.patch.object(
                VALIDATOR,
                "production_headers",
                return_value=(Path("/future/wave/header.h"),),
            ),
        ):
            self.assertEqual(expected, VALIDATOR.report_bytes())

    def test_live_architecture_scan_remains_enabled(self) -> None:
        self.assertEqual([], VALIDATOR.architecture_leaks())


if __name__ == "__main__":
    unittest.main()
