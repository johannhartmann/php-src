--TEST--
Native direct boolean results feed conditions through the generic call path
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
function w11p_direct_bool_leaf(int $value): bool
{
    return (bool) ($value % 2);
}

function w11p_direct_bool_root(): string
{
    return w11p_direct_bool_leaf(5) ? 'odd' : 'even';
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-direct-bool-condition.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_direct_bool_root',
        'repeat' => 20,
    ],
);
$execution = $result['execution'];
$performance = $execution['performance'];
printf(
    "%s return=%s runs=%d active=%d vm=%d execute_ex=%d handlers=%d calls=%d typed=%d frame=%s\n",
    $result['status'],
    $execution['return_value'],
    $execution['executions'],
    $execution['entry_active_calls'],
    $execution['vm_handler_calls'],
    $execution['execute_ex_calls'],
    $execution['opline_handler_calls'],
    $performance['direct_call_sites'],
    $performance['direct_typed_body_sites'],
    $performance['direct_call_frame_bytes'] > 0 ? 'yes' : 'no',
);
?>
--EXPECT--
accepted return=odd runs=20 active=0 vm=0 execute_ex=0 handlers=0 calls=1 typed=0 frame=yes
