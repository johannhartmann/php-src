--TEST--
Native typed component boolean results feed conditions without frame materialization
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
function w14_bool_condition_leaf(int $value): bool
{
    if ($value < 0) {
        return false;
    }
    return true;
}

function w14_bool_condition_root(int $value): string
{
    return w14_bool_condition_leaf($value) ? 'odd' : 'even';
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w14-typed-component-bool-condition.php',
    [5],
    [
        'wave' => 11,
        'function' => 'w14_bool_condition_root',
        'repeat' => 20,
    ],
);
$execution = $result['execution'];
$performance = $execution['performance'];
printf(
    "%s return=%s runs=%d active=%d vm=%d execute_ex=%d handlers=%d typed=%d frame=%d\n",
    $result['status'],
    $execution['return_value'],
    $execution['executions'],
    $execution['entry_active_calls'],
    $execution['vm_handler_calls'],
    $execution['execute_ex_calls'],
    $execution['opline_handler_calls'],
    $performance['direct_typed_body_sites'],
    $performance['direct_call_frame_bytes'],
);
?>
--EXPECT--
accepted return=odd runs=20 active=0 vm=0 execute_ex=0 handlers=0 typed=1 frame=0
