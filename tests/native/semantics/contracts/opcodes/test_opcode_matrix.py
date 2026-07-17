#!/usr/bin/env python3

from __future__ import annotations

import csv
import importlib.util
import io
import json
import subprocess
import tempfile
import unittest
from copy import deepcopy
from pathlib import Path


THIS_DIR = Path(__file__).resolve().parent
ROOT = THIS_DIR.parents[4]
GENERATOR_PATH = ROOT / "scripts/native/semantics/generate-opcode-matrix.py"
DOC_DIR = ROOT / "docs/native-engine/semantics/opcodes"

spec = importlib.util.spec_from_file_location("opcode_matrix_generator", GENERATOR_PATH)
generator = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(generator)


def semantic_override(opcode: str, line: int, *, pure: bool = False) -> dict[str, object]:
    if pure:
        effects: list[str] = []
        reads: list[str] = []
        writes: list[str] = []
        ownership: list[str] = []
        reason = "The cited definition proves pure/no-memory/no-ownership behavior."
        result = "none"
    else:
        effects = ["read_memory", "write_memory"]
        reads = ["frame.temps"]
        writes = ["frame.temps"]
        ownership = ["borrow", "produce_owned"]
        reason = "The cited handler defines ordinary operand and result behavior."
        result = "temporary"
    return {
        "result": result,
        "may_throw": "never",
        "may_bailout": "never",
        "may_call_php": "never",
        "may_run_dtor": "never",
        "reference_semantics": "The cited handler has no reference-specific path.",
        "observer_boundary": "never",
        "interrupt_boundary": "never",
        "existing_jit_support": "none",
        "planned_znmir_lowering": "runtime.test",
        "n0_x64": "not_started",
        "n0_a64": "not_started",
        "special_cases": [],
        "effect_ids": effects,
        "read_domains": reads,
        "write_domains": writes,
        "ownership_actions": ownership,
        "barrier_ids": [],
        "tests": ["contract.fixture", "opcode.%s" % opcode],
        "source_refs": [{
            "path": "Zend/zend_vm_def.h",
            "symbol": opcode,
            "start_line": line,
            "end_line": line,
        }],
        "reason": reason,
    }


class MiniRepository:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.header = root / "Zend/zend_vm_opcodes.h"
        self.vm_def = root / "Zend/zend_vm_def.h"
        self.overrides = root / "docs/native-engine/semantics/opcodes/opcode-overrides.json"
        self.output = self.overrides.parent
        for relative in generator.SOURCE_PATHS:
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture\n", encoding="utf-8")
        self.write_header()
        self.write_vm_def()
        self.write_overrides()

    def write_header(self, extra: str = "") -> None:
        self.header.write_text(
            "#ifndef ZEND_VM_OPCODES_H\n"
            "#define ZEND_VM_OPCODES_H\n"
            "#define ZEND_NOP 0\n"
            "#define ZEND_ADD 1\n"
            "#define ZEND_OP_DATA 2\n"
            + extra
            + "#define ZEND_VM_LAST_OPCODE 2\n"
            "#endif\n",
            encoding="utf-8",
        )

    def write_vm_def(self) -> None:
        self.vm_def.write_text(
            "ZEND_VM_HOT_HANDLER(0, ZEND_NOP, ANY, ANY)\n"
            "{\n"
            "    ZEND_VM_NEXT_OPCODE();\n"
            "}\n"
            "ZEND_VM_COLD_CONSTCONST_HANDLER(1, ZEND_ADD, CONST|TMPVARCV, CONST|TMPVARCV, SPEC(COMMUTATIVE))\n"
            "{\n"
            "    add_function(EX_VAR(opline->result.var), GET_OP1_ZVAL_PTR(BP_VAR_R), GET_OP2_ZVAL_PTR(BP_VAR_R));\n"
            "}\n"
            "ZEND_VM_HOT_TYPE_SPEC_HANDLER(ZEND_ADD, (op1_info == foo(1, 2)), ZEND_ADD_LONG, CONST|TMPVARCV, CONST|TMPVARCV, SPEC(COMMUTATIVE))\n"
            "{\n"
            "    ZVAL_LONG(EX_VAR(opline->result.var), 1);\n"
            "}\n"
            "ZEND_VM_DEFINE_OP(2, ZEND_OP_DATA);\n",
            encoding="utf-8",
        )

    def write_overrides(self, mutate=None) -> None:
        document = {
            "format_version": generator.FORMAT_VERSION,
            "source_commit": generator.SOURCE_COMMIT,
            "opcodes": {
                "ZEND_NOP": semantic_override("ZEND_NOP", 1, pure=True),
                "ZEND_ADD": semantic_override("ZEND_ADD", 5),
                "ZEND_OP_DATA": semantic_override("ZEND_OP_DATA", 13, pure=True),
            },
        }
        if mutate:
            mutate(document)
        self.overrides.parent.mkdir(parents=True, exist_ok=True)
        self.overrides.write_text(json.dumps(document), encoding="utf-8")

    def build(self) -> dict[str, object]:
        return generator.build_matrix(self.root, self.header, self.vm_def, self.overrides)


class OpcodeMatrixTests(unittest.TestCase):
    maxDiff = None

    def test_actual_repository_covers_every_executable_opcode_once(self) -> None:
        matrix = generator.build_matrix(
            ROOT,
            ROOT / "Zend/zend_vm_opcodes.h",
            ROOT / "Zend/zend_vm_def.h",
            DOC_DIR / "opcode-overrides.json",
        )
        rows = matrix["opcodes"]
        self.assertEqual(210, len(rows))
        self.assertEqual(210, len({row["opcode"] for row in rows}))
        self.assertEqual(210, len({row["number"] for row in rows}))
        self.assertEqual([45, 79], matrix["reserved_opcode_numbers"])
        self.assertEqual({"name": "ZEND_VM_LAST_OPCODE", "value": 211}, matrix["sentinel"])
        self.assertEqual("ZEND_OP_DATA", next(row["opcode"] for row in rows if row["number"] == 137))
        self.assertNotIn("ZEND_VM_LAST_OPCODE", {row["opcode"] for row in rows})
        self.assertEqual(set(generator.SOURCE_PATHS), {item["path"] for item in matrix["source_files"]})
        self.assertTrue(all(row["n0_x64"] == "not_started" for row in rows))
        self.assertTrue(all(row["n0_a64"] == "not_started" for row in rows))

    def test_high_risk_annotations_have_direct_handler_evidence(self) -> None:
        matrix = generator.build_matrix(
            ROOT,
            ROOT / "Zend/zend_vm_opcodes.h",
            ROOT / "Zend/zend_vm_def.h",
            DOC_DIR / "opcode-overrides.json",
        )
        lines = (ROOT / "Zend/zend_vm_def.h").read_text(encoding="utf-8").splitlines()
        marker_groups = {
            "destructor": ("zval_ptr_dtor", "_dtor", "zend_object_release", "zend_array_destroy", "zend_string_release", "FREE_OP", "GC_DELREF", "OBJ_RELEASE"),
            "observer": ("OBSERVER",),
            "interrupt": ("INTERRUPT_CHECK", "zend_fcall_interrupt", "zend_interrupt_function", "ZEND_VM_JMP(", "ZEND_VM_SET_OPCODE(", "ZEND_VM_LOOP_INTERRUPT_CHECK"),
            "fiber": ("fiber",),
            "zts": ("EG(",),
        }
        for row in matrix["opcodes"]:
            vm_refs = [
                ref for ref in row["source_refs"] if ref["path"] == "Zend/zend_vm_def.h"
            ]
            body = "\n".join(
                "\n".join(lines[ref["start_line"] - 1:ref["end_line"]])
                for ref in vm_refs
            )
            for tag, markers in marker_groups.items():
                if tag in row["special_cases"]:
                    self.assertTrue(any(marker.lower() in body.lower() for marker in markers), (row["opcode"], tag))
            if "reference" in row["special_cases"]:
                self.assertTrue(
                    "REF" in row["opcode"] or any(
                        marker.lower() in body.lower()
                        for marker in ("IS_REFERENCE", "Z_ISREF", "Z_REF", "reference", "INDIRECT", "DEREF", "SEPARATE")
                    ),
                    row["opcode"],
                )
                self.assertIn("heap.reference", row["read_domains"])
                self.assertTrue(row["ownership_actions"])
        fast_call = next(row for row in matrix["opcodes"] if row["opcode"] == "ZEND_FAST_CALL")
        self.assertNotIn("heap.array", fast_call["read_domains"])

    def test_dispatch_targets_are_structural_and_semantically_anchored(self) -> None:
        matrix = generator.build_matrix(
            ROOT,
            ROOT / "Zend/zend_vm_opcodes.h",
            ROOT / "Zend/zend_vm_def.h",
            DOC_DIR / "opcode-overrides.json",
        )
        rows = {row["opcode"]: row for row in matrix["opcodes"]}
        add = rows["ZEND_ADD"]
        self.assertEqual("conditional", add["may_run_dtor"])
        self.assertIn("zend_add_helper", {ref["symbol"] for ref in add["source_refs"]})
        self.assertIn(
            {"kind": "helper", "target": "zend_add_helper"},
            [{"kind": item["kind"], "target": item["target"]}
             for item in add["operands"]["dispatches"]],
        )
        pre_dec = rows["ZEND_PRE_DEC_STATIC_PROP"]
        self.assertIn("ZEND_PRE_INC_STATIC_PROP", {ref["symbol"] for ref in pre_dec["source_refs"]})
        self.assertEqual(rows["ZEND_PRE_INC_STATIC_PROP"]["may_throw"], pre_dec["may_throw"])
        self.assertTrue(
            set(rows["ZEND_PRE_INC_STATIC_PROP"]["barrier_ids"])
            <= set(pre_dec["barrier_ids"])
        )

    def test_jit_support_uses_compiler_region_cases_not_name_occurrence(self) -> None:
        matrix = generator.build_matrix(
            ROOT,
            ROOT / "Zend/zend_vm_opcodes.h",
            ROOT / "Zend/zend_vm_def.h",
            DOC_DIR / "opcode-overrides.json",
        )
        for row in matrix["opcodes"]:
            refs = row["source_refs"]
            function = any(
                ref["path"] == "ext/opcache/jit/zend_jit.c"
                and 1416 <= ref["start_line"] <= 2707
                for ref in refs
            )
            trace = any(
                (ref["path"] == "ext/opcache/jit/zend_jit_trace.c" and 4099 <= ref["start_line"] <= 7427)
                or (ref["path"] == "ext/opcache/jit/zend_jit_ir.c" and 17126 <= ref["start_line"] <= 17295)
                for ref in refs
            )
            support = row["existing_jit_support"]
            if support == "both":
                self.assertTrue(function and trace, row["opcode"])
            elif support == "function":
                self.assertTrue(function and not trace, row["opcode"])
            elif support == "trace":
                self.assertTrue(trace and not function, row["opcode"])
            elif support == "partial":
                self.assertTrue(any(ref["path"].startswith("ext/opcache/jit/") for ref in refs), row["opcode"])
            else:
                self.assertFalse(any(ref["path"].startswith("ext/opcache/jit/") for ref in refs), row["opcode"])

    def test_actual_checked_outputs_are_current_and_equivalent(self) -> None:
        matrix = generator.build_matrix(
            ROOT,
            ROOT / "Zend/zend_vm_opcodes.h",
            ROOT / "Zend/zend_vm_def.h",
            DOC_DIR / "opcode-overrides.json",
        )
        payloads = generator.output_payloads(matrix)
        generator.check_outputs(DOC_DIR, payloads)
        json_rows = json.loads(payloads["opcode-matrix.json"])["opcodes"]
        csv_rows = list(csv.DictReader(io.StringIO(payloads["opcode-matrix.csv"].decode("utf-8"))))
        self.assertEqual(len(json_rows), len(csv_rows))
        for json_row, csv_row in zip(json_rows, csv_rows):
            decoded: dict[str, object] = {}
            for field in generator.CSV_FIELDS:
                value: object = csv_row[field]
                if field == "number":
                    value = int(value)
                elif isinstance(json_row[field], (dict, list)):
                    value = json.loads(value)
                decoded[field] = value
            self.assertEqual({field: json_row[field] for field in generator.CSV_FIELDS}, decoded)

    def test_schema_and_generator_require_the_same_opcode_fields(self) -> None:
        schema = json.loads((DOC_DIR / "opcode-matrix.schema.json").read_text(encoding="utf-8"))
        required = set(schema["$defs"]["opcode"]["required"])
        self.assertEqual({"opcode", "number", "operands", *generator.SEMANTIC_FIELDS}, required)
        self.assertEqual(generator.JIT_SUPPORT, set(schema["$defs"]["jitSupport"]["enum"]))
        self.assertEqual(generator.EFFECT_IDS, set(schema["$defs"]["effect"]["enum"]))
        self.assertEqual(generator.MEMORY_DOMAINS, set(schema["$defs"]["domain"]["enum"]))
        self.assertEqual(generator.OWNERSHIP_ACTIONS, set(schema["$defs"]["ownership"]["enum"]))
        self.assertEqual(generator.BARRIER_IDS, set(schema["$defs"]["barrier"]["enum"]))
        self.assertEqual(
            {"handler_style", "op1", "op2", "extended", "spec", "variants", "dispatches"},
            set(schema["$defs"]["operands"]["required"]),
        )

    def test_parser_accepts_multiple_handler_styles_and_balanced_variant_condition(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = MiniRepository(Path(directory))
            handlers = generator.parse_vm_def(fixture.vm_def)
            self.assertEqual("ZEND_VM_HOT_HANDLER", handlers["ZEND_NOP"]["operands"]["handler_style"])
            self.assertEqual("ZEND_VM_COLD_CONSTCONST_HANDLER", handlers["ZEND_ADD"]["operands"]["handler_style"])
            variants = handlers["ZEND_ADD"]["operands"]["variants"]
            self.assertEqual(1, len(variants))
            self.assertEqual("(op1_info == foo(1, 2))", variants[0]["condition"])
            self.assertEqual("ZEND_VM_DEFINE_OP", handlers["ZEND_OP_DATA"]["operands"]["handler_style"])

    def test_dispatched_target_without_source_anchor_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = MiniRepository(Path(directory))
            fixture.vm_def.write_text(
                fixture.vm_def.read_text(encoding="utf-8").replace(
                    "    add_function(EX_VAR(opline->result.var), GET_OP1_ZVAL_PTR(BP_VAR_R), GET_OP2_ZVAL_PTR(BP_VAR_R));",
                    "    ZEND_VM_DISPATCH_TO_HANDLER(ZEND_NOP);",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(generator.MatrixError, "dispatched handler source reference"):
                fixture.build()

    def test_generation_is_byte_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = MiniRepository(Path(directory))
            first = generator.output_payloads(fixture.build())
            second = generator.output_payloads(fixture.build())
            self.assertEqual(first, second)

    def test_missing_override_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = MiniRepository(Path(directory))
            fixture.write_overrides(lambda doc: doc["opcodes"].pop("ZEND_ADD"))
            with self.assertRaisesRegex(generator.MatrixError, "override coverage mismatch"):
                fixture.build()

    def test_unknown_override_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = MiniRepository(Path(directory))
            fixture.write_overrides(
                lambda doc: doc["opcodes"].update({"ZEND_FAKE": deepcopy(doc["opcodes"]["ZEND_NOP"])})
            )
            with self.assertRaisesRegex(generator.MatrixError, "override coverage mismatch"):
                fixture.build()

    def test_duplicate_opcode_number_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = MiniRepository(Path(directory))
            fixture.header.write_text(
                fixture.header.read_text(encoding="utf-8").replace("#define ZEND_ADD 1", "#define ZEND_ADD 0"),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(generator.MatrixError, "duplicate opcode number"):
                fixture.build()

    def test_duplicate_opcode_name_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = MiniRepository(Path(directory))
            fixture.header.write_text(
                fixture.header.read_text(encoding="utf-8").replace("#define ZEND_OP_DATA 2", "#define ZEND_ADD 2"),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(generator.MatrixError, "duplicate opcode name"):
                fixture.build()

    def test_new_opcode_without_handler_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = MiniRepository(Path(directory))
            fixture.header.write_text(
                fixture.header.read_text(encoding="utf-8")
                .replace("#define ZEND_OP_DATA 2", "#define ZEND_OP_DATA 2\n#define ZEND_NEW_OPCODE 3")
                .replace("#define ZEND_VM_LAST_OPCODE 2", "#define ZEND_VM_LAST_OPCODE 3"),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(generator.MatrixError, "handler coverage mismatch"):
                fixture.build()

    def test_stale_source_anchor_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = MiniRepository(Path(directory))
            def mutate(doc):
                doc["opcodes"]["ZEND_ADD"]["source_refs"][0]["start_line"] = 99
                doc["opcodes"]["ZEND_ADD"]["source_refs"][0]["end_line"] = 99
            fixture.write_overrides(mutate)
            with self.assertRaisesRegex(generator.MatrixError, "line range exceeds"):
                fixture.build()

    def test_source_anchor_with_missing_symbol_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = MiniRepository(Path(directory))
            def mutate(doc):
                doc["opcodes"]["ZEND_ADD"]["source_refs"][0]["symbol"] = "ZEND_REMOVED"
            fixture.write_overrides(mutate)
            with self.assertRaisesRegex(generator.MatrixError, "symbol is absent"):
                fixture.build()

    def test_placeholder_in_required_field_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = MiniRepository(Path(directory))
            fixture.write_overrides(
                lambda doc: doc["opcodes"]["ZEND_ADD"].update({"reason": "Decide later."})
            )
            with self.assertRaisesRegex(generator.MatrixError, "prohibited placeholder"):
                fixture.build()

    def test_claimed_jit_support_requires_jit_source_anchor(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = MiniRepository(Path(directory))
            fixture.write_overrides(
                lambda doc: doc["opcodes"]["ZEND_ADD"].update({"existing_jit_support": "function"})
            )
            with self.assertRaisesRegex(generator.MatrixError, "JIT source reference"):
                fixture.build()

    def test_suspend_effect_requires_barrier_and_ownership(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = MiniRepository(Path(directory))
            def missing_barrier(doc):
                doc["opcodes"]["ZEND_ADD"]["effect_ids"].append("suspend")
            fixture.write_overrides(missing_barrier)
            with self.assertRaisesRegex(generator.MatrixError, "effect suspend lacks barrier"):
                fixture.build()
        with tempfile.TemporaryDirectory() as directory:
            fixture = MiniRepository(Path(directory))
            def missing_ownership(doc):
                row = doc["opcodes"]["ZEND_ADD"]
                row["effect_ids"].append("suspend")
                row["barrier_ids"].append("suspend")
                row["ownership_actions"] = []
            fixture.write_overrides(missing_ownership)
            with self.assertRaisesRegex(generator.MatrixError, "effect suspend lacks ownership"):
                fixture.build()

    def test_check_mode_rejects_stale_output(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = MiniRepository(Path(directory))
            payloads = generator.output_payloads(fixture.build())
            generator.write_outputs(fixture.output, payloads)
            (fixture.output / "opcode-matrix.csv").write_text("stale\n", encoding="utf-8")
            with self.assertRaisesRegex(generator.MatrixError, "stale"):
                generator.check_outputs(fixture.output, payloads)

    def test_cli_reports_success_for_checked_in_matrix(self) -> None:
        completed = subprocess.run(
            [str(GENERATOR_PATH), "--check"],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(0, completed.returncode, completed.stderr)
        self.assertIn("210 opcodes", completed.stdout)


if __name__ == "__main__":
    unittest.main()
