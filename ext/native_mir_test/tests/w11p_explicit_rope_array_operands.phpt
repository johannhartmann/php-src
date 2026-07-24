--TEST--
Native rope and array-construction slow paths consume explicit MIR operands
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
function selected($left, $right): int
{
    $text = "x{$left}y{$right}z";
    $values = [1, $left, 'k' => $right, ...[4, 5]];
    return strlen($text)
        + $values[0]
        + $values[1]
        + $values['k']
        + $values[2]
        + $values[3];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-explicit-rope-array.php',
    [2, 3],
    ['wave' => 11, 'function' => 'selected', 'repeat' => 10],
);
printf(
    "%s return=%d executions=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
);
?>
--EXPECT--
accepted return=20 executions=10 vm=0 execute_ex=0 handler=0
