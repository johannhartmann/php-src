--TEST--
Native compile/dump dispatches wave 11 through the W11 lowering pipeline
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
function w11p_dump_dispatch(): int
{
    return 42;
}
PHP,
    'w11p-compile-dump-dispatch.php',
    ['wave' => 11, 'function' => 'w11p_dump_dispatch'],
);

printf(
    "%s diagnostic=%s w11=%s complete=%s\n",
    $result['status'],
    $result['diagnostics'][0]['code'],
    $result['diagnostics'][0]['message'] === 'W11 lowering completed'
        ? 'yes'
        : 'no',
    is_string($result['mir']) && str_ends_with($result['mir'], "end\n")
        ? 'yes'
        : 'no',
);
?>
--EXPECT--
accepted diagnostic=MIRL0000 w11=yes complete=yes
