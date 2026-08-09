--TEST--
Native boxed scalar array reads compose with conditions and arithmetic
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$result = native_mir_test_compile_execute(
    <<<'PHP'
<?php
function array_value_scalar_consumers(): int
{
    $values = ['count' => 40, 'enabled' => true];

    if ($values['enabled']) {
        return $values['count'] + 2;
    }

    return -1;
}
PHP,
    'w11p-inline-array-value-scalar-consumers.php',
    [],
    [
        'wave' => 11,
        'function' => 'array_value_scalar_consumers',
        'repeat' => 30,
    ],
);
$literal = native_mir_test_compile_execute(
    <<<'PHP'
<?php
function literal_array_value_scalar_consumer(int $key): int
{
    return [40, 41, 42][$key] + 1;
}
PHP,
    'w11p-inline-literal-array-value-scalar-consumer.php',
    [1],
    [
        'wave' => 11,
        'function' => 'literal_array_value_scalar_consumer',
        'repeat' => 30,
    ],
);
$literal_loop = native_mir_test_compile_execute(
    <<<'PHP'
<?php
function literal_array_value_scalar_loop(int $count): int
{
    $result = 0;
    for ($index = 0; $index < $count; $index++) {
        $key = $index & 7;
        $result += [1, 2, 3, 4, 5, 6, 7, 8][$key];
    }
    return $result;
}
PHP,
    'w11p-inline-literal-array-value-scalar-loop.php',
    [32],
    [
        'wave' => 11,
        'function' => 'literal_array_value_scalar_loop',
        'repeat' => 30,
    ],
);
printf(
    "%s return=%d runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
printf(
    "%s literal=%d runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $literal['status'],
    $literal['execution']['return_value'],
    $literal['execution']['executions'],
    $literal['execution']['vm_handler_calls'],
    $literal['execution']['execute_ex_calls'],
    $literal['execution']['opline_handler_calls'],
    $literal['execution']['entry_active_calls'],
);
printf(
    "%s literal_loop=%d runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $literal_loop['status'],
    $literal_loop['execution']['return_value'],
    $literal_loop['execution']['executions'],
    $literal_loop['execution']['vm_handler_calls'],
    $literal_loop['execution']['execute_ex_calls'],
    $literal_loop['execution']['opline_handler_calls'],
    $literal_loop['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=42 runs=30 vm=0 execute_ex=0 handler=0 active=0
accepted literal=42 runs=30 vm=0 execute_ex=0 handler=0 active=0
accepted literal_loop=144 runs=30 vm=0 execute_ex=0 handler=0 active=0
