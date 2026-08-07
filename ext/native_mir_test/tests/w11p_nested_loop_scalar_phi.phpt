--TEST--
Native nested loop scalar PHI survives an interrupt statepoint fast path
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
function w11p_nested_loop_scalar_phi(): string
{
    for ($count = 0; $count < 1; $count++) {
        for ($index = 0; $index < 1;) {
            for (; $index < 1;) {
                for (; $index < 1; $index++) {
                }
            }
        }
        for ($index = 0; $index < 1; $index++) {
        }
    }

    return 'done';
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-nested-loop-scalar-phi.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_nested_loop_scalar_phi',
        'repeat' => 20,
    ],
);

printf(
    "%s return=%s runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return="done" runs=20 vm=0 execute_ex=0 handler=0 active=0
