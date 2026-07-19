<?php

/** @generate-class-entries */

/**
 * Compile source without executing it and return a canonical W03/W04 MIR result.
 *
 * @return array{
 *     schema_version: int,
 *     wave?: 4,
 *     status: "accepted"|"rejected"|"error",
 *     phase: "compile"|"ssa"|"lowering"|"verify"|"dump"|"complete",
 *     source: array{filename: string, byte_length: int, source_id: string},
 *     diagnostics: list<array{
 *         stage: "compile"|"ssa"|"MIRL"|"MIRV"|"bridge",
 *         code: string,
 *         message: string,
 *         opline: ?int
 *     }>,
 *     mir: ?string
 * }
 * @param array{
 *     function?: ?string,
 *     diagnostic_limit?: int,
 *     wave?: 3|4,
 *     arena_chunk_size?: int,
 *     fault?: ?string
 * } $options
 */
function native_mir_test_compile_dump(
    string $source,
    string $filename,
    array $options = [],
): array {}
