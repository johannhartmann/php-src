--TEST--
Native MIR W08 propagates an exception across native user frames
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
function throws_native(): int
{
    return intdiv(1, 0);
}

function catches_user(): int
{
    try {
        return throws_native();
    } catch (DivisionByZeroError) {
        return 9;
    }
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w08-multiframe-exception.php',
    [],
    ['wave' => 8, 'function' => 'catches_user'],
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
accepted returned return=9 exception=0 bailout=0 vm=0 execute_ex=0 handler=0
