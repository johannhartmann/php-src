--TEST--
Native dynamic by-reference checks remain bound to the outer call across nested calls
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
function w11p_byref_index(): int
{
    return 0;
}

function w11p_byref_increment(int &$value): int
{
    return ++$value;
}

function w11p_nested_dynamic_byref(bool $dynamic): int
{
    $values = [41];
    $callable = $dynamic ? 'w11p_byref_increment' : 'w11p_byref_increment';
    $result = $callable($values[w11p_byref_index()]);
    return $result + $values[0] - 42;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-nested-dynamic-byref-check.php',
    [true],
    [
        'wave' => 11,
        'function' => 'w11p_nested_dynamic_byref',
        'repeat' => 10,
    ],
);
$execution = $result['execution'];
printf(
    "%s return=%d runs=%d codeunits=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $execution['return_value'],
    $execution['executions'],
    $execution['native_codeunits'],
    $execution['vm_handler_calls'],
    $execution['execute_ex_calls'],
    $execution['opline_handler_calls'],
    $execution['entry_active_calls'],
);
?>
--EXPECT--
accepted return=42 runs=10 codeunits=3 vm=0 execute_ex=0 handler=0 active=0
