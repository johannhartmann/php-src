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

function caught_second_internal(): int
{
    try {
        intdiv(1, 0);
    } catch (TypeError) {
        return 1;
    } catch (DivisionByZeroError) {
        return 2;
    }
    return 3;
}

function caught_variable()
{
    try {
        intdiv(1, 0);
    } catch (DivisionByZeroError $e) {
        return $e->getCode();
    }
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

$second = native_mir_test_compile_execute(
    $source,
    'w08-internal-exception.php',
    [],
    ['wave' => 8, 'function' => 'caught_second_internal'],
);
printf(
    "%s %s return=%s exception=%d bailout=%d vm=%d execute_ex=%d handler=%d\n",
    $second['status'],
    $second['execution']['status'],
    json_encode($second['execution']['return_value']),
    $second['execution']['exception'],
    $second['execution']['bailout'],
    $second['execution']['vm_handler_calls'],
    $second['execution']['execute_ex_calls'],
    $second['execution']['opline_handler_calls'],
);

$variable = native_mir_test_compile_execute(
    $source,
    'w08-internal-exception.php',
    [],
    ['wave' => 8, 'function' => 'caught_variable'],
);
printf(
    "%s %s return=%s exception=%d bailout=%d vm=%d execute_ex=%d handler=%d\n",
    $variable['status'],
    $variable['execution']['status'],
    json_encode($variable['execution']['return_value']),
    $variable['execution']['exception'],
    $variable['execution']['bailout'],
    $variable['execution']['vm_handler_calls'],
    $variable['execution']['execute_ex_calls'],
    $variable['execution']['opline_handler_calls'],
);
?>
--EXPECT--
accepted returned return=7 exception=0 bailout=0 vm=0 execute_ex=0 handler=0
accepted returned return=2 exception=0 bailout=0 vm=0 execute_ex=0 handler=0
accepted returned return=0 exception=0 bailout=0 vm=0 execute_ex=0 handler=0
