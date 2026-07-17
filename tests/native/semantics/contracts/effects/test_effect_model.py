"""Contract tests for the native effect and ownership model."""

from __future__ import annotations

import copy
import importlib.util
import json
import subprocess
import sys
import unittest
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[5]
VALIDATOR_PATH = ROOT / "scripts/native/semantics/validate-effect-model.py"
MODEL_PATH = ROOT / "docs/native-engine/semantics/effects/effect-model.json"
SCHEMA_PATH = ROOT / "docs/native-engine/semantics/effects/effect-model.schema.json"
FIXTURE_DIR = Path(__file__).resolve().parent / "fixtures"

SPEC = importlib.util.spec_from_file_location("effect_model_validator", VALIDATOR_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot import validator at {VALIDATOR_PATH}")
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)


def _step(node: Any, token: str) -> Any:
    if isinstance(node, dict):
        return node[token]
    if isinstance(node, list):
        for item in node:
            if isinstance(item, dict) and item.get("id") == token:
                return item
    raise KeyError(token)


def apply_operation(model: dict[str, Any], operation: dict[str, Any]) -> None:
    path = operation["path"]
    target: Any = model
    for token in path:
        target = _step(target, token)
    if operation["op"] == "append":
        target.append(operation["value"])
    elif operation["op"] == "remove":
        target.remove(operation["value"])
    else:
        raise AssertionError(f"unsupported fixture operation: {operation['op']}")


class EffectModelContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.schema = VALIDATOR.load_json(SCHEMA_PATH)
        cls.model = VALIDATOR.load_json(MODEL_PATH)

    def test_canonical_model_is_valid(self) -> None:
        VALIDATOR.validate_model(copy.deepcopy(self.model), self.schema)

    def test_command_line_check_is_successful(self) -> None:
        result = subprocess.run(
            [sys.executable, str(VALIDATOR_PATH), "--check"],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("effect model validation passed", result.stdout)

    def test_negative_fixtures_fail_for_the_declared_reason(self) -> None:
        fixtures = sorted(FIXTURE_DIR.glob("*.json"))
        self.assertGreaterEqual(len(fixtures), 4)
        for path in fixtures:
            with self.subTest(fixture=path.name):
                with path.open(encoding="utf-8") as handle:
                    fixture = json.load(handle)
                mutated = copy.deepcopy(self.model)
                for operation in fixture["operations"]:
                    apply_operation(mutated, operation)
                with self.assertRaises(VALIDATOR.ValidationError) as raised:
                    VALIDATOR.validate_model(mutated, self.schema)
                self.assertIn(fixture["expected_error"], str(raised.exception))

    def test_schema_rejects_unregistered_top_level_fields(self) -> None:
        mutated = copy.deepcopy(self.model)
        mutated["unregistered"] = True
        with self.assertRaisesRegex(VALIDATOR.ValidationError, "unexpected properties"):
            VALIDATOR.validate_model(mutated, self.schema)


if __name__ == "__main__":
    unittest.main()
