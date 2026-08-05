--TEST--
Native try-continue loop preserves local initialization
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
function w11p_try_continue_local_initialization(int $a, int $b): int
{
    $i = $j = 0;
    do {
        $i++;
        try {
            continue;
        } catch (Exception) {
        }
        do {
            $j++;
        } while ($j < $b);
    } while ($i < $a);

    return $i * 10 + $j;
}
PHP,
    'w11p-try-continue-local-initialization.php',
    [5, 6],
    [
        'wave' => 11,
        'function' => 'w11p_try_continue_local_initialization',
        'repeat' => 2,
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
?>
--EXPECT--
accepted return=50 runs=2 vm=0 execute_ex=0 handler=0 active=0
