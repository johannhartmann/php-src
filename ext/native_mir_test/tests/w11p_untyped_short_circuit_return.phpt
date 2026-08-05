--TEST--
Native untyped short-circuit PHI retains its boxed return terminator
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$source = <<<'PHP'
<?php
function w11p_untyped_return_helper()
{
    return true;
}

function w11p_untyped_short_circuit_return(int $value)
{
    return ($value > 0xdead && w11p_untyped_return_helper())
        || ($value < 0xbeef && w11p_untyped_return_helper());
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-untyped-short-circuit-return.php',
    [0xcccc],
    [
        'wave' => 11,
        'function' => 'w11p_untyped_short_circuit_return',
        'repeat' => 10,
    ],
);

printf(
    "%s return=%s runs=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'] ? 'true' : 'false',
    $result['execution']['executions'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=false runs=10 active=0
