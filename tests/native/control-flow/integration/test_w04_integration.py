"""W04 source-wiring and MIR integration tests."""

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


class W04IntegrationTests(unittest.TestCase):
    def test_source_wiring_profile_and_goldens(self) -> None:
        validator.validate()

    def test_config_is_generated_from_source_manifest(self) -> None:
        manifest = validator.load(validator.SOURCES)
        self.assertEqual(
            validator.config_sources(),
            sorted(manifest["w04_production_sources"]),
        )

    def test_goldens_cover_branch_phi_loop_and_edge_statepoints(self) -> None:
        index = validator.load(validator.GOLDENS)
        totals = {
            key: sum(entry["cfg"][key] for entry in index["cases"].values())
            for key in ("blocks", "edges", "phis", "backedges", "statepoints")
        }
        self.assertTrue(all(value > 0 for value in totals.values()))
        for entry in index["cases"].values():
            path = ROOT / entry["path"]
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), entry["sha256"])

    def test_true_successor_precedes_false_successor(self) -> None:
        mir = (ROOT / "tests/native/control-flow/integration/goldens/if_else_int.znmir").read_text(encoding="utf-8")
        self.assertIn("block b0 function f0 predecessors [] successors [b1, b2]", mir)
        self.assertIn("instruction i3 block b0 opcode cond_branch", mir)

    def test_interrupt_edge_has_closed_semantics(self) -> None:
        mir = (ROOT / "tests/native/control-flow/integration/goldens/while_loop_counter.znmir").read_text(encoding="utf-8")
        statepoint = next(line for line in mir.splitlines() if "opcode statepoint" in line and "effects 0x0400" in line)
        self.assertIn("reads 0x00004008", statepoint)
        self.assertIn("writes 0x00004000", statepoint)
        self.assertIn("barriers 0x61", statepoint)


if __name__ == "__main__":
    unittest.main()
