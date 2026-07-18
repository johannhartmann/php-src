<?php

/** @generate-class-entries */

/**
 * Compile source without executing it and return a canonical W03 MIR result.
 *
 * @return array{
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
 *     mir: ?string
 * }
 */
function native_mir_test_compile_dump(
    string $source,
    string $filename,
    array $options = [],
): array {}
