--TEST--
Native MIR W08 polls Zend interrupts at source-backed loop backedges
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
if (PHP_OS_FAMILY === 'Windows') {
    die('skip Unix timeout semantics are required');
}
?>
--FILE--
<?php
$source = <<<'PHP'
<?php
function native_interrupt_loop(): int
{
    while (true) {
    }
    return 0;
}
PHP;

set_time_limit(1);
$result = native_mir_test_compile_execute(
    $source,
    'w08-interrupt-poll.php',
    [],
    ['wave' => 8, 'function' => 'native_interrupt_loop'],
);
set_time_limit(0);

printf(
    "%s %s exception=%d bailout=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $result['execution']['status'],
    $result['execution']['exception'],
    $result['execution']['bailout'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
);
?>
--EXPECTF--
Fatal error: Maximum execution time of 1 second exceeded in w08-interrupt-poll.php on line %d
error bailout exception=0 bailout=1 vm=0 execute_ex=0 handler=0
