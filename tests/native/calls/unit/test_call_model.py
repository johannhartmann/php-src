from __future__ import annotations

import json
import re
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
MODEL = ROOT / "Zend/Native/Calls/Model/zend_mir_call_model.c"
MODULE = ROOT / "Zend/Native/MIR/Core/zend_mir_module.c"
COMPILER = ROOT / "Zend/zend_compile.c"
FRONTEND = ROOT / "Zend/Native/Lowering/Frontend/zend_mir_zend_source.c"
W01_MATRIX = ROOT / "docs/native-engine/semantics/opcodes/opcode-matrix.json"
SOURCE = Path(__file__).with_name("test_call_contract.c")


class CallModelTests(unittest.TestCase):
    def test_public_contract_compiles_and_runs_under_c11(self) -> None:
        with tempfile.TemporaryDirectory(prefix="w05-call-unit-") as directory:
            executable = Path(directory) / "test_call_contract"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I.",
                    str(SOURCE),
                    "-o",
                    str(executable),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(executable)], cwd=ROOT, check=True)

    def test_planner_precedes_w04_and_all_mutation(self) -> None:
        text = MODEL.read_text(encoding="utf-8")
        planner = text.index("code = zend_mir_w05_plan_calls(")
        w04 = text.index("w04 = zend_mir_lower_w04_zend_source(")
        emitter = text.index("zend_mir_w05_emit_calls(&plan")
        self.assertLess(planner, w04)
        self.assertLess(w04, emitter)

    def test_plan_is_frozen_and_codegen_debt_stays_open(self) -> None:
        text = MODEL.read_text(encoding="utf-8")
        self.assertIn("plan->public_plan.complete = true;", text)
        self.assertIn("plan->public_plan.immutable = true;", text)
        self.assertIn("result.codegen_eligible = false;", text)
        self.assertIn("ZEND_MIR_W05_REQUIRED_DEBTS", text)

    def test_module_publication_uses_staging_and_atomic_commit(self) -> None:
        text = MODULE.read_text(encoding="utf-8")
        self.assertIn("zend_mir_core_call_staging", text)
        self.assertIn("zend_mir_core_commit_call_model", text)
        self.assertIn("staging->committed = true;", text)
        self.assertIn("free(module->call_staging.targets);", text)

    def test_call_results_and_source_order_are_published_exactly(self) -> None:
        model = MODEL.read_text(encoding="utf-8")
        module = MODULE.read_text(encoding="utf-8")
        self.assertIn("zend_mir_w05_result_value", model)
        self.assertIn("site.result_id = plan->results[index];", model)
        self.assertIn("zend_mir_w05_verify_source_order", model)
        self.assertIn("zend_mir_core_append_calls_before", module)
        self.assertIn("site->result_id, &result_index", module)

    def test_declaration_identity_accepts_exact_recursive_self_calls(self) -> None:
        model = MODEL.read_text(encoding="utf-8")
        frontend = FRONTEND.read_text(encoding="utf-8")
        self.assertIn("zend_mir_frontend_declaration_id", frontend)
        self.assertIn("&function->op_array == caller", frontend)
        self.assertNotIn(
            "resolved.function_symbol_id\n\t\t\t\t\t\t== context->function_symbol_id",
            model,
        )

    def test_model_contains_no_runtime_or_target_backend(self) -> None:
        text = MODEL.read_text(encoding="utf-8").lower()
        for token in (
            "zend_execute",
            "execute_ex",
            "zend_vm_call_opcode_handler",
            "mir_interpret",
            "tpde",
            "x86",
            "aarch64",
        ):
            self.assertNotIn(token, text)

    def test_source_sequence_validation_proves_send_and_lifo_nesting(self) -> None:
        text = MODEL.read_text(encoding="utf-8")
        start = text.index("zend_mir_w05_source_sequence(")
        end = text.index("\nstatic ", start + 1)
        validator = text[start:end]
        self.assertIn("source_opcode_count(calls->context)", validator)
        self.assertIn("send_opline_index", validator)
        self.assertIn("site->parent_call_site_id", validator)
        self.assertIn("ZEND_MIR_SOURCE_CALL_SITE_NESTED", validator)
        self.assertIn("stack[stack_count - 1]", validator)
        self.assertIn("seen_arguments", validator)
        self.assertIn("argument->flags != 0", validator)

    def test_nested_call_result_is_rejected_before_mutation(self) -> None:
        model = MODEL.read_text(encoding="utf-8")
        frontend = FRONTEND.read_text(encoding="utf-8")
        self.assertIn(
            "zend_mir_frontend_w05_argument_is_call_result", frontend
        )
        self.assertIn(
            "return ZEND_MIRL_W05_UNSUPPORTED_RESULT;", model
        )

    def test_normalized_named_call_needs_no_compiler_side_channel(self) -> None:
        compiler = COMPILER.read_text(encoding="utf-8")
        frontend = FRONTEND.read_text(encoding="utf-8")
        self.assertNotIn("ZEND_SEND_SYNTACTIC_NAMED", compiler)
        self.assertNotIn("ZEND_MIR_ZEND_SEND_SYNTACTIC_NAMED", frontend)
        self.assertNotIn(
            "ZEND_MIR_SOURCE_CALL_ARGUMENT_SYNTACTIC_NAMED",
            frontend,
        )
        self.assertIn("argument->flags = 0;", frontend)
        self.assertIn("argument->flags != 0", frontend)

    def test_parameter_modes_are_scalable_and_ordered(self) -> None:
        frontend = FRONTEND.read_text(encoding="utf-8")
        model = MODEL.read_text(encoding="utf-8")
        self.assertIn("zend_mir_frontend_append_parameter_modes", frontend)
        self.assertIn("parameter_mode_capacity", frontend)
        self.assertIn("mode->ordinal = argument;", frontend)
        self.assertIn(
            "zend_mir_w05_target_parameter_modes_are_by_value", model
        )
        self.assertNotIn("num_args > 64", model)
        self.assertNotIn("by_ref_mask", model)

    def test_final_verifier_precedes_determinism_check(self) -> None:
        model = MODEL.read_text(encoding="utf-8")
        lower = model[model.index("zend_mir_lower_direct_user_calls(") :]
        final_verify = lower.index("zend_mir_w05_verify_final_composition(")
        fingerprint = lower.index("zend_mir_w05_build_fingerprints(")
        recompute = lower.index(
            "zend_mir_w05_build_fingerprints(", fingerprint + 1
        )
        self.assertEqual(
            [final_verify, fingerprint, recompute],
            sorted([final_verify, fingerprint, recompute]),
        )
        self.assertNotIn("receipt", lower)
        self.assertNotIn("module_ops.verify_stage1(", lower)
        self.assertNotIn("module_ops.verify_stage2(", lower)
        self.assertNotIn("zend_mir_verify_w04_control_flow(", lower)
        self.assertIn("zend_mir_dump_text(view, &writer", model)
        self.assertIn(
            "zend_mir_w05_verify_final_structural(", model
        )
        self.assertIn("zend_mir_w05_verify_final_scalar(", model)
        self.assertIn(
            "zend_mir_w05_verify_final_control_flow(", model
        )
        self.assertIn("zend_mir_verify_w05_calls(", model)
        self.assertIn("source->opcode_at", model)
        self.assertIn("source->edge_at", model)
        self.assertIn("source_calls->call_argument_at", model)
        self.assertIn("zend_mir_lowering_result_is_w04_failure_atomic(&w04)", model)

    def test_fingerprint_has_independent_words(self) -> None:
        model = MODEL.read_text(encoding="utf-8")
        self.assertIn("uint32_t words[4];", model)
        self.assertIn("static const uint32_t domains[4]", model)
        self.assertNotIn("module_writer.hash", model)
        self.assertIn("recomputed_module_fingerprint", model)

    def test_call_effect_summary_is_a_w01_sequence_superset(self) -> None:
        text = MODEL.read_text(encoding="utf-8")
        matrix = json.loads(W01_MATRIX.read_text(encoding="utf-8"))
        accepted = {
            "ZEND_INIT_FCALL",
            "ZEND_SEND_VAL",
            "ZEND_SEND_VAL_EX",
            "ZEND_SEND_VAR",
            "ZEND_SEND_VAR_EX",
            "ZEND_DO_UCALL",
            "ZEND_DO_FCALL",
        }
        entries = [
            entry for entry in matrix["opcodes"] if entry["opcode"] in accepted
        ]
        self.assertEqual({entry["opcode"] for entry in entries}, accepted)

        def macro(name: str) -> str:
            start = text.index(f"#define {name}")
            endings = [
                position
                for marker in ("\n#define ", "\nstatic ")
                if (position := text.find(marker, start + 1)) >= 0
            ]
            self.assertTrue(endings, name)
            return text[start : min(endings)]

        effect_body = macro("W05_EFFECTS")
        barrier_body = macro("W05_BARRIERS")
        read_body = macro("W05_READS")
        write_body = macro("W05_WRITES")
        effects = {
            token.lower()
            for token in re.findall(r"ZEND_MIR_EFFECT_([A-Z_]+)", effect_body)
        }
        barriers = {
            token.lower()
            for token in re.findall(r"ZEND_MIR_BARRIER_([A-Z_]+)", barrier_body)
        }
        def domain_name(token: str) -> str:
            namespace, name = token.lower().split("_", 1)
            return f"{namespace}.{name}"

        reads = {
            domain_name(token)
            for token in re.findall(
                r"ZEND_MIR_MEMORY_DOMAIN_([A-Z_]+)", read_body
            )
            if "_" in token
        }
        writes = set(reads) if "W05_READS" in write_body else set()
        writes.update(
            domain_name(token)
            for token in re.findall(
                r"ZEND_MIR_MEMORY_DOMAIN_([A-Z_]+)", write_body
            )
            if "_" in token
        )
        self.assertFalse("~" in write_body, "W05 writes must not subtract W01 domains")
        self.assertTrue(
            set().union(*(set(entry["effect_ids"]) for entry in entries))
            <= effects
        )
        self.assertTrue(
            set().union(*(set(entry["barrier_ids"]) for entry in entries))
            <= barriers
        )
        self.assertTrue(
            set().union(*(set(entry["read_domains"]) for entry in entries))
            <= reads
        )
        self.assertTrue(
            set().union(*(set(entry["write_domains"]) for entry in entries))
            <= writes
        )


if __name__ == "__main__":
    unittest.main()
