--TEST--
Native baseline passes explicit list, unset dimension, and foreach-free operands
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
function selected(array $input): int
{
    $values = $input;
    [$first, $second] = $values;
    unset($values[1]);
    $sum = 0;
    foreach ($values as $key => $value) {
        $sum += $key + $value;
    }
    return $first + $second + $sum;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-explicit-list-dimension-operands.php',
    [[2, 3, 5]],
    ['wave' => 11, 'function' => 'selected', 'repeat' => 10],
);
printf(
    "status=%s return=%d executions=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
);
?>
--EXPECT--
status=accepted return=14 executions=10 vm=0 execute_ex=0 handler=0
