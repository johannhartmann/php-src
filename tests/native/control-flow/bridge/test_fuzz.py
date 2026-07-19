"""Determinism and accounting tests for the W04 fixed-seed fuzzer."""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[4]
FUZZ_PATH = ROOT / "tests/native/control-flow/fuzz/run_fuzz.py"


def load_fuzzer() -> Any:
    specification = importlib.util.spec_from_file_location("w04_fuzz_test", FUZZ_PATH)
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


fuzzer = load_fuzzer()


class FuzzTests(unittest.TestCase):
    def test_fixed_seed_is_repeatable_and_covers_every_category(self) -> None:
        first = fuzzer.run(20260719, 250)
        second = fuzzer.run(20260719, 250)
        self.assertEqual(first, second)
        self.assertEqual(first["status"], "pass")
        self.assertEqual(
            set(first["categories"]),
            {
                "source",
                "options",
                "diagnostic_limit",
                "bailout",
                "manifest_mutation",
            },
        )
        self.assertTrue(
            all(category["total"] == 50 for category in first["categories"].values())
        )

    def test_seed_and_case_bounds_are_enforced(self) -> None:
        for seed, cases in ((-1, 1), (0, 0), (0, 1_000_001), (True, 1)):
            with self.subTest(seed=seed, cases=cases):
                with self.assertRaises(fuzzer.FuzzError):
                    fuzzer.run(seed, cases)


if __name__ == "__main__":
    unittest.main()
