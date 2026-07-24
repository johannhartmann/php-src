--TEST--
Native concat slow paths consume explicit MIR operands
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
function selected(&$left, $right): int
{
    return strlen($left . $right);
}
PHP;

$left = 'ab';
$result = native_mir_test_compile_execute(
    $source,
    'w11p-explicit-concat.php',
    [&$left, 'cd'],
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
accepted return=4 executions=10 vm=0 execute_ex=0 handler=0
