--TEST--
Native Fiber forced close selects only active nested finally regions
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
function w12_fiber_forced_close_nested_finally_root(): array
{
    $trace = [];
    $fiber = new Fiber(function () use (&$trace): void {
        try {
            try {
                try {
                    $trace[] = 'start';
                    Fiber::suspend('ready');
                } catch (Throwable $unused) {
                    $trace[] = 'caught-inner';
                }
            } catch (Throwable $unused) {
                $trace[] = 'caught-outer';
            } finally {
                $trace[] = 'finally-inner';
            }
        } finally {
            $trace[] = 'finally-outer';
        }

        try {
            Fiber::suspend('unreachable');
        } finally {
            $trace[] = 'finally-later';
        }
    });

    $first = $fiber->start();
    $fiber = null;
    gc_collect_cycles();
    return [$first, $trace];
}
PHP,
    'w12-fiber-forced-close-nested-finally.php',
    [],
    [
        'wave' => 11,
        'function' => 'w12_fiber_forced_close_nested_finally_root',
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
accepted result=["ready",["start","finally-inner","finally-outer"]] vm=0 execute_ex=0 handler=0 active=0
