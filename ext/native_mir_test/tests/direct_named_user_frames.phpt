--TEST--
Native universal user calls place known named arguments without expansion helpers
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
function native_named_target(int $left, int $middle, int $right = 0): int
{
    return $left + $middle + $right;
}

function native_named_root(int $first, int $second, int $third): int
{
    return native_named_target(
        right: $third,
        left: $first,
        middle: $second,
    );
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'direct-named-user-frames.php',
    [10, 20, 12],
    [
        'function' => 'native_named_root',
        'repeat' => 10,
    ],
);
$execution = $result['execution'];
$performance = $execution['performance'];
printf(
    "%s return=%d runs=%d direct=%d helpers=%d "
    . "vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $execution['return_value'],
    $execution['executions'],
    $performance['direct_call_sites'],
    $performance['inner_call_runtime_helper_calls'],
    $execution['vm_handler_calls'],
    $execution['execute_ex_calls'],
    $execution['opline_handler_calls'],
    $execution['entry_active_calls'],
);
?>
--EXPECT--
accepted return=42 runs=10 direct=1 helpers=0 vm=0 execute_ex=0 handler=0 active=0
