<?php

/** @generate-class-entries */

/**
 * Compile source without executing it and return a canonical W03/W04/W05 MIR result.
 *
 * Result shape:
 * array{
 *     schema_version: int,
 *     wave?: 4|5,
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
 *     wave?: 3|4|5,
 *     arena_chunk_size?: int,
 *     compiler_mode?: "ignore_user_functions",
 *     fault?: null|"compile_bailout"|"ssa_failure"|"lower_failure"|
 *         "module_oom"|"planner_allocation"|"target_snapshot"|
 *         "argument_table"|"frame_state"|"call_record"|"finalize_failure"|
 *         "stage1_verifier_failure"|"stage2_verifier_failure"|
 *         "call_verifier_failure"|"dump_failure"
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
