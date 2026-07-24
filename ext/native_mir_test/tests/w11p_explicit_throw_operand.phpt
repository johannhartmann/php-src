--TEST--
Native throw consumes an explicit MIR operand without source-opline decoding
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
function selected(Throwable $throwable): int
{
    try {
        throw $throwable;
    } catch (RuntimeException $caught) {
        return $caught->getCode();
    }
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-explicit-throw.php',
    [new RuntimeException('explicit', 42)],
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
accepted return=42 executions=10 vm=0 execute_ex=0 handler=0
