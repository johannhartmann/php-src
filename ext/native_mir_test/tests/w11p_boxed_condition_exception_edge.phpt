--TEST--
Native boxed condition guards preserve exception CFG successors
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
function w11p_boxed_condition_exception_edge($take): int
{
    try {
        if ($take) {
            return 2;
        }
    } finally {
        intdiv(6, 3);
    }

    return 3;
}
PHP,
    'w11p-boxed-condition-exception-edge.php',
    ['yes'],
    [
        'wave' => 11,
        'function' => 'w11p_boxed_condition_exception_edge',
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
accepted return=2 runs=10 active=0
