--TEST--
Native MIR W08 obeys the platform ABI for wide mixed helper calls
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
function native_abi_probe(int $value)
{
    echo $value;
    return $value;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w08-abi.php',
    [37],
    [
        'wave' => 8,
        'function' => 'native_abi_probe',
        'abi_probe' => true,
        'repeat' => 100,
    ],
);
printf(
    "%s %s value=%d executions=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $result['execution']['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
);
?>
--EXPECT--
accepted returned value=37 executions=100 vm=0 execute_ex=0 handler=0
