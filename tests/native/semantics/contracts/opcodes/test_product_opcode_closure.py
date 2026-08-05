#!/usr/bin/env python3
"""Contract tests for the live native product opcode closure proof."""

from __future__ import annotations

import importlib.util
import shutil
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[5]
SCRIPT = ROOT / "scripts/native/verify-opcode-closure.py"


def load_verifier():
    spec = importlib.util.spec_from_file_location("verify_opcode_closure", SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import {SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


VERIFIER = load_verifier()


class ProductOpcodeClosureTest(unittest.TestCase):
    def copy_product_inputs(self, destination: Path) -> None:
        for relative in VERIFIER.INPUT_PATHS:
            source = ROOT / relative
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)

    def test_live_product_closes_all_212_active_opcodes(self) -> None:
        evidence = VERIFIER.build_evidence(ROOT)

        self.assertEqual(212, evidence["opcode_set"]["active_count"])
        self.assertEqual(212, evidence["summary"]["covered_count"])
        self.assertEqual(
            set(range(214)) - {45, 79},
            {record["number"] for record in evidence["opcodes"]},
        )
        self.assertTrue(
            all(value == 0 for value in evidence["summary"]["hard_zero_metrics"].values())
        )
        self.assertFalse(evidence["historical_wave_artifacts_used"])

    def test_semantic_special_cases_are_explicit_and_testable(self) -> None:
        evidence = VERIFIER.build_evidence(ROOT)
        by_opcode = {record["opcode"]: record for record in evidence["opcodes"]}

        expected = {
            "ZEND_HANDLE_EXCEPTION": (
                "native_exception_route",
                "executable_tpde",
            ),
            "ZEND_USER_OPCODE": (
                "native_user_opcode_callback",
                "bounded_native_runtime",
            ),
            "ZEND_CALL_TRAMPOLINE": (
                "native_call_trampoline_normalization",
                "bounded_native_runtime",
            ),
        }
        for opcode, (classification, implementation) in expected.items():
            with self.subTest(opcode=opcode):
                record = by_opcode[opcode]
                self.assertEqual(classification, record["classification"])
                self.assertEqual(implementation, record["implementation"])
                self.assertGreaterEqual(len(record["source_refs"]), 4)
                self.assertTrue(all(ref["line"] > 0 for ref in record["source_refs"]))

        self.assertEqual("atomic_owner_sequence", by_opcode["ZEND_OP_DATA"]["classification"])

    def test_missing_executable_mapping_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            sandbox = Path(temporary)
            self.copy_product_inputs(sandbox)
            lowering = sandbox / VERIFIER.VALUE_LOWERING
            source = lowering.read_text(encoding="utf-8")
            self.assertIn("case ZEND_TYPE_ASSERT:", source)
            lowering.write_text(
                source.replace("case ZEND_TYPE_ASSERT:", "case ZEND_NOT_AN_OPCODE:", 1),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(
                VERIFIER.ClosureError,
                "active opcodes without a current product path: ZEND_TYPE_ASSERT",
            ):
                VERIFIER.build_evidence(sandbox)

    def test_missing_special_semantic_anchor_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            sandbox = Path(temporary)
            self.copy_product_inputs(sandbox)
            calls = sandbox / VERIFIER.RUNTIME_CALLS
            source = calls.read_text(encoding="utf-8")
            anchor = "action = handler(execute_data);"
            self.assertIn(anchor, source)
            calls.write_text(source.replace(anchor, "action = 0;", 1), encoding="utf-8")

            with self.assertRaisesRegex(
                VERIFIER.ClosureError,
                "required product anchor is absent",
            ):
                VERIFIER.build_evidence(sandbox)

    def test_vm_handler_acceptance_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            sandbox = Path(temporary)
            self.copy_product_inputs(sandbox)
            compiler = sandbox / VERIFIER.COMPILER
            source = compiler.read_text(encoding="utf-8")
            compiler.write_text(
                source + "\nvoid forbidden(zend_execute_data *execute_data) {\n"
                "\texecute_data->opline->handler(execute_data);\n}\n",
                encoding="utf-8",
            )

            with self.assertRaisesRegex(
                VERIFIER.ClosureError,
                "forbidden VM acceptance is present",
            ):
                VERIFIER.build_evidence(sandbox)


if __name__ == "__main__":
    unittest.main()
