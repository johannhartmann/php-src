--TEST--
Native MIR W08 restores nested user and internal call state after bailout
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
if (!function_exists('zend_trigger_bailout')) {
    die('skip zend_test is not available');
}
?>
--INI--
display_errors=0
log_errors=0
--FILE--
<?php
$source = <<<'PHP'
<?php
function native_bailout_inner(): int
{
    zend_trigger_bailout();
    return 1;
}

function native_bailout_outer(): int
{
    return native_bailout_inner();
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w08-bailout.php',
    [],
    ['wave' => 8, 'function' => 'native_bailout_outer', 'stack_probe' => true],
);

printf(
    "%s %s bailout=%d exception=%d frame=%d probes=%d "
        . "vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $result['execution']['status'],
    $result['execution']['bailout'],
    $result['execution']['exception'],
    $result['execution']['frame_chain_valid'],
    count($result['execution']['stack_trace']),
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
);
?>
--EXPECT--
error bailout bailout=1 exception=0 frame=1 probes=1 vm=0 execute_ex=0 handler=0
