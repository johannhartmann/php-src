--TEST--
Native Fiber forced close bypasses catch and enters finally
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
function w12_fiber_forced_close_finally_root(): array
{
    $trace = [];
    $fiber = new Fiber(function () use (&$trace): void {
        try {
            $trace[] = 'start';
            Fiber::suspend('ready');
        } catch (Throwable $unused) {
            $trace[] = 'caught';
        } finally {
            $trace[] = 'finally';
        }
    });

    $first = $fiber->start();
    $fiber = null;
    gc_collect_cycles();
    return [$first, $trace];
}
PHP,
    'w12-fiber-forced-close-finally.php',
    [],
    [
        'wave' => 11,
        'function' => 'w12_fiber_forced_close_finally_root',
        'repeat' => 10,
    ],
);

printf(
    "%s result=%s vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted result=["ready",["start","finally"]] vm=0 execute_ex=0 handler=0 active=0
