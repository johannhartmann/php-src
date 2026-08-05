--TEST--
Native direct user calls materialize variadic arguments in generated frames
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
function w11p_variadic_target(int $base, int ...$values): int
{
    return $base + $values[0] + $values[1];
}

function w11p_variadic_frame(int $base, int $first, int $second): int
{
    return w11p_variadic_target($base, $first, $second);
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-inline-variadic-user-frames.php',
    [10, 20, 12],
    [
        'wave' => 11,
        'function' => 'w11p_variadic_frame',
        'repeat' => 5,
    ],
);
$execution = $result['execution'];
$performance = $execution['performance'];
printf(
    "%s return=%d runs=%d direct=%d helpers=%d allocations=%d catchers=%d "
    . "vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $execution['return_value'],
    $execution['executions'],
    $performance['direct_call_sites'],
    $performance['inner_call_runtime_helper_calls'],
    $performance['inner_call_heap_allocations'],
    $performance['inner_call_catcher_boundaries'],
    $execution['vm_handler_calls'],
    $execution['execute_ex_calls'],
    $execution['opline_handler_calls'],
);
?>
--EXPECT--
accepted return=42 runs=5 direct=1 helpers=0 allocations=0 catchers=0 vm=0 execute_ex=0 handler=0
