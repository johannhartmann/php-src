--TEST--
Native assignment-op and increment slow paths consume explicit MIR operands
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
function selected(&$value, $delta): int
{
    $value += $delta;
    return ++$value;
}
PHP;

$value = 2;
$result = native_mir_test_compile_execute(
    $source,
    'w11p-explicit-mutation.php',
    [&$value, 3],
    ['wave' => 11, 'function' => 'selected', 'repeat' => 10],
);
printf(
    "%s return=%d executions=%d vm=%d execute_ex=%d handler=%d value=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $value,
);
?>
--EXPECT--
accepted return=42 executions=10 vm=0 execute_ex=0 handler=0 value=42
