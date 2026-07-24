--TEST--
Native binary and cast slow paths consume explicit MIR operands
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
    return (int) ($left + $right);
}
PHP;

$left = '2.5';
$result = native_mir_test_compile_execute(
    $source,
    'w11p-explicit-binary-cast.php',
    [&$left, '3.5'],
    ['wave' => 11, 'function' => 'selected', 'repeat' => 10],
);
printf(
    "%s return=%d executions=%d vm=%d execute_ex=%d handler=%d left=%s\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $left,
);
?>
--EXPECT--
accepted return=6 executions=10 vm=0 execute_ex=0 handler=0 left=2.5
