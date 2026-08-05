--TEST--
Native W11 uses source-backed ownership for unpacked and extra user arguments
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$result = native_mir_test_compile_execute(
    <<<'PHP'
<?php
function w11p_source_backed_target(mixed $first): array
{
    return func_get_args();
}

function w11p_source_backed_unpack_arguments(array $arguments): int
{
    return count(w11p_source_backed_target(...$arguments));
}
PHP,
    'w11p-source-backed-unpack-arguments.php',
    [[1, 2, 3]],
    [
        'wave' => 11,
        'function' => 'w11p_source_backed_unpack_arguments',
        'repeat' => 10,
    ],
);
printf(
    "%s return=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=3 vm=0 execute_ex=0 handler=0 active=0
