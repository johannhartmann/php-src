--TEST--
Native compile/dump lowers through the production pipeline
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_dump')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$result = native_mir_test_compile_dump(
    <<<'PHP'
<?php
function native_dump_dispatch(): int
{
    return 42;
}
PHP,
    'compile-dump-dispatch.php',
    ['function' => 'native_dump_dispatch'],
);

printf(
    "%s diagnostic=%s lowered=%s complete=%s\n",
    $result['status'],
    $result['diagnostics'][0]['code'],
    $result['diagnostics'][0]['message'] === 'lowering completed'
        ? 'yes'
        : 'no',
    is_string($result['mir']) && str_ends_with($result['mir'], "end\n")
        ? 'yes'
        : 'no',
);
?>
--EXPECT--
accepted diagnostic=MIRL0000 lowered=yes complete=yes
