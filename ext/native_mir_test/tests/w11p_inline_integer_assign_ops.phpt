--TEST--
Native baseline executes exact integer assignment and bitwise operations inline
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
function w11p_integer_assign_ops(int $count): int
{
    $value = 3;
    $captured = 0;
    for ($index = 0; $index < $count; $index++) {
        $captured = ($value += 5);
        $captured = ($value ^= 11);
        $captured = ($value &= 0x7fffffff);
        $captured = ($value |= 16);
        $captured = ($value -= 3);
    }
    return $captured;
}
PHP;

function w11p_integer_assign_ops_oracle(int $count): int
{
    $value = 3;
    $captured = 0;
    for ($index = 0; $index < $count; $index++) {
        $captured = ($value += 5);
        $captured = ($value ^= 11);
        $captured = ($value &= 0x7fffffff);
        $captured = ($value |= 16);
        $captured = ($value -= 3);
    }
    return $captured;
}

$expected = w11p_integer_assign_ops_oracle(1000);
$result = native_mir_test_compile_execute(
    $source,
    'w11p-inline-integer-assign-ops.php',
    [1000],
    [
        'wave' => 11,
        'function' => 'w11p_integer_assign_ops',
        'repeat' => 10,
    ],
);
printf(
    "%s return=%d expected=%d runs=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $expected,
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
);
?>
--EXPECT--
accepted return=8019 expected=8019 runs=10 vm=0 execute_ex=0 handler=0
