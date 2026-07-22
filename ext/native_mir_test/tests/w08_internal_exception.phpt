--TEST--
Native MIR W08 catches an exception raised by a direct internal call
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
function caught_internal(): int
{
    try {
        intdiv(1, 0);
    } catch (DivisionByZeroError) {
        return 7;
    }
    return 0;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w08-internal-exception.php',
    [],
    ['wave' => 8, 'function' => 'caught_internal'],
);
printf(
    "%s %s return=%s exception=%d bailout=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $result['execution']['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['exception'],
    $result['execution']['bailout'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
);
?>
--EXPECT--
accepted returned return=7 exception=0 bailout=0 vm=0 execute_ex=0 handler=0
