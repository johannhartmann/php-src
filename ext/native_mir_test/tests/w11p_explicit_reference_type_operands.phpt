--TEST--
Native reference and type slow paths consume explicit MIR operands
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
    $left =& $right;
    unset($right);
    return is_int($left) ? $left : 0;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-explicit-reference-type.php',
    [1, 7],
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
accepted return=7 executions=10 vm=0 execute_ex=0 handler=0
