"""Unit tests for strict W03 corpus manifest validation."""

from __future__ import annotations

import hashlib
import importlib.util
import unittest
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[4]
VALIDATOR_PATH = ROOT / "tests/native/lowering/corpus/validate_manifest.py"
REJECTED_SOURCE = (
    ROOT / "tests/native/lowering/corpus/rejected/missing_proofs.php"
)


def load_validator() -> Any:
    specification = importlib.util.spec_from_file_location(
        "w03_manifest_validator_test", VALIDATOR_PATH
    )
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


validator = load_validator()


def rejected_case() -> dict[str, Any]:
    relative = REJECTED_SOURCE.relative_to(ROOT).as_posix()
    return {
        "case_id": "missing_proofs",
        "deferred_wave": None,
        "disposition": "rejected",
        "expected_mirl": "MIRL0002",
        "family": "missing_proofs",
        "function": "w03_rejected_missing_proofs",
        "golden_path": None,
        "golden_sha256": None,
        "reference_path": relative,
        "repeat_calls": [1],
        "source_path": relative,
        "source_sha256": hashlib.sha256(REJECTED_SOURCE.read_bytes()).hexdigest(),
    }


class ManifestValidatorTests(unittest.TestCase):
    def test_valid_rejected_case_is_accepted(self) -> None:
        validator.validate_case(rejected_case(), 0)

    def test_repeat_calls_must_not_be_empty(self) -> None:
        case = rejected_case()
        case["repeat_calls"] = []
        with self.assertRaisesRegex(validator.ManifestError, "repeat_calls"):
            validator.validate_case(case, 0)

    def test_expected_mirl_must_be_a_registered_w03_code(self) -> None:
        case = rejected_case()
        case["expected_mirl"] = "MIRL9999"
        with self.assertRaisesRegex(validator.ManifestError, "expected_mirl"):
            validator.validate_case(case, 0)

    def test_rejected_case_must_use_rejected_fixture_directory(self) -> None:
        case = rejected_case()
        accepted = ROOT / "tests/native/lowering/corpus/accepted/constants.php"
        relative = accepted.relative_to(ROOT).as_posix()
        case["source_path"] = relative
        case["reference_path"] = relative
        case["source_sha256"] = hashlib.sha256(accepted.read_bytes()).hexdigest()
        with self.assertRaisesRegex(validator.ManifestError, "rejected fixture"):
            validator.validate_case(case, 0)


if __name__ == "__main__":
    unittest.main()
