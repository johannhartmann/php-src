"""Unit tests for the strict W01 semantic manifest validator."""

from __future__ import annotations

import copy
import sys
import tempfile
import unittest
from pathlib import Path


SEMANTICS_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SEMANTICS_ROOT))

import validate_manifest as validator  # noqa: E402


class ManifestValidationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.repository_root = Path(self.temporary_directory.name)
        self.source_path = Path("tests/native/semantics/corpus/phpt/all_semantics.phpt")
        source = self.repository_root / self.source_path
        source.parent.mkdir(parents=True)
        source.write_text("--TEST--\nall\n--FILE--\n<?php echo 'ok'; ?>\n--EXPECT--\nok\n", encoding="utf-8")
        boundaries = [
            {
                "boundary": boundary,
                "mechanism": "source-proven test mechanism",
                "source_anchors": [self.source_path.as_posix() + ":1-6"],
            }
            for boundary in sorted(validator.REQUIRED_BOUNDARIES.values())
        ]
        fixture = {
            "determinism_constraints": sorted(validator.ALLOWED_DETERMINISM_CONSTRAINTS),
            "fixture_id": "all_semantics",
            "forced_boundaries": boundaries,
            "kind": "new_phpt",
            "observable_channels": ["exit_status", "stderr", "stdout"],
            "opcode_families": sorted(validator.REQUIRED_OPCODE_FAMILIES),
            "opcodes": [],
            "owner": "W01-E",
            "reference_provenance_required": True,
            "repeat_calls": list(range(1, 11)),
            "required_extensions": ["zend_test"],
            "required_modes": ["cli", "phpt", "zend_test_observer", "zts"],
            "semantic_risks": sorted(validator.REQUIRED_SEMANTIC_RISKS),
            "source_path": self.source_path.as_posix(),
        }
        self.document = {
            "fixtures": [fixture],
            "format_version": validator.FORMAT_VERSION,
            "php_src_semantics_commit": validator.PHP_SRC_SEMANTICS_COMMIT,
            "required_opcode_families": sorted(validator.REQUIRED_OPCODE_FAMILIES),
            "required_semantic_risks": sorted(validator.REQUIRED_SEMANTIC_RISKS),
            "wave_base_commit": validator.WAVE_BASE_COMMIT,
        }

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def assert_invalid(self, document, message_fragment: str) -> None:
        with self.assertRaisesRegex(validator.ManifestError, message_fragment):
            validator.validate_document(document, self.repository_root)

    def test_positive_complete_mini_manifest(self) -> None:
        self.assertIs(self.document, validator.validate_document(self.document, self.repository_root))

    def test_missing_fixture_is_rejected(self) -> None:
        document = copy.deepcopy(self.document)
        document["fixtures"][0]["source_path"] = "tests/native/semantics/corpus/phpt/missing.phpt"
        self.assert_invalid(document, "source does not exist")

    def test_duplicate_id_is_rejected(self) -> None:
        document = copy.deepcopy(self.document)
        document["fixtures"].append(copy.deepcopy(document["fixtures"][0]))
        self.assert_invalid(document, "fixture_id values must be unique")

    def test_incomplete_family_coverage_is_rejected(self) -> None:
        document = copy.deepcopy(self.document)
        document["fixtures"][0]["opcode_families"].remove("strings")
        self.assert_invalid(document, "missing opcode family coverage")

    def test_unstable_environment_assumption_is_rejected(self) -> None:
        document = copy.deepcopy(self.document)
        document["fixtures"][0]["determinism_constraints"] = ["uses_wall_clock"]
        self.assert_invalid(document, "unstable or unsupported assumption")

    def test_implicit_reference_provenance_is_rejected(self) -> None:
        document = copy.deepcopy(self.document)
        document["fixtures"][0]["reference_provenance_required"] = False
        self.assert_invalid(document, "explicit reference provenance")

    def test_absolute_checked_in_path_is_rejected(self) -> None:
        document = copy.deepcopy(self.document)
        document["fixtures"][0]["source_path"] = "/tmp/fixture.phpt"
        self.assert_invalid(document, "repository-relative")

    def test_missing_forced_boundary_is_rejected(self) -> None:
        document = copy.deepcopy(self.document)
        document["fixtures"][0]["forced_boundaries"] = document["fixtures"][0]["forced_boundaries"][1:]
        self.assert_invalid(document, "lacks forced boundary")

    def test_incomplete_repeat_call_attribution_is_rejected(self) -> None:
        document = copy.deepcopy(self.document)
        document["fixtures"][0]["repeat_calls"] = list(range(1, 10))
        self.assert_invalid(document, "calls 1 through 10")


if __name__ == "__main__":
    unittest.main()
