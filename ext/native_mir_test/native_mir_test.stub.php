<?php

/** @generate-class-entries */

/**
 * Compile source without executing it and return a canonical W03-W07 MIR result.
 *
 * Result shape:
 * array{
 *     schema_version: int,
 *     wave?: 4|5|6|7,
 *     status: "accepted"|"rejected"|"error",
 *     phase: "compile"|"ssa"|"lowering"|"verify"|"dump"|"complete",
 *     source: array{filename: string, byte_length: int, source_id: string},
 *     diagnostics: list<array{
 *         stage: "compile"|"ssa"|"MIRL"|"MIRV"|"bridge",
 *         code: string,
 *         message: string,
 *         opline: ?int
 *     }>,
 *     source_opcodes: list<string>,
 *     mir: ?string
 * }
 *
 * Options shape:
 * array{
 *     function?: ?string,
 *     diagnostic_limit?: int,
 *     wave?: 3|4|5|6|7,
 *     arena_chunk_size?: int,
 *     compiler_mode?: "ignore_user_functions",
 *     fault?: null|"compile_bailout"|"ssa_failure"|"lower_failure"|
 *         "module_oom"|"planner_allocation"|"target_snapshot"|
 *         "argument_table"|"frame_state"|"call_record"|"finalize_failure"|
 *         "stage1_verifier_failure"|"stage2_verifier_failure"|
 *         "structural_verifier_failure"|"scalar_verifier_failure"|
 *         "control_flow_verifier_failure"|"call_verifier_failure"|
 *         "fingerprint_recompute_failure"|"value_inventory"|"value_plan"|
 *         "value_storage"|"value_reference"|"value_alias"|"value_event"|
 *         "value_separation"|"value_call_transfer"|
 *         "value_verifier_failure"|"dump_failure"
 * }
 *
 * @return array
 * @param array $options
 */
function native_mir_test_compile_dump(
    string $source,
    string $filename,
    array $options = [],
): array {}

/**
 * Compile source through SSA and verified ZNMIR, publish native code, and
 * execute it over real Zend frames without entering a VM opcode handler.
 *
 * @return array
 * @param list<null|bool|int|float> $arguments
 * @param array $options
 */
function native_mir_test_compile_execute(
    string $source,
    string $filename,
    array $arguments = [],
    array $options = [],
): array {}
