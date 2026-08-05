--TEST--
Native finally calls preserve exception CFG successors
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
function w11p_finally_call_exception_edge(): int
{
    try {
        return 1;
    } catch (Exception) {
        return -1;
    } finally {
        intdiv(6, 3);
    }
}
PHP,
    'w11p-finally-call-exception-edge.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_finally_call_exception_edge',
        'repeat' => 10,
    ],
);

printf(
    "%s return=%s runs=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['executions'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=1 runs=10 active=0
