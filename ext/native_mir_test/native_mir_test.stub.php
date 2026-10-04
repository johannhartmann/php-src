<?php

/** @generate-class-entries */

/**
 * Compile source through the production lowering without executing it and
 * return the canonical MIR result.
 *
 * Result shape:
 * array{
 *     schema_version: int,
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
 *     arena_chunk_size?: int,
 *     compiler_mode?: "ignore_user_functions",
 *     stack_probe?: true,
 *     fault?: null|"compile_bailout"|"ssa_failure"|"lower_failure"|
 *         "module_oom"|"finalize_failure"|"stage1_verifier_failure"|
 *         "stage2_verifier_failure"|"dump_failure"|"mapping_failure"|
 *         "entry_publish_failure"
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
 * @param list<mixed> $arguments
 * @param array $options
 */
function native_mir_test_compile_execute(
    string $source,
    string $filename,
    array $arguments = [],
    array $options = [],
): array {}

/**
 * Return the number of published native frames visible to the platform stack
 * unwinder. This test-only probe returns zero outside native execution.
 */
function native_mir_test_unwind_probe(): int {}

/**
 * Advance the native call-cache epoch as the end of a request does: published
 * fast call sites must be re-armed or resolved again. Test-only.
 */
function native_mir_test_call_cache_invalidate(): void {}
