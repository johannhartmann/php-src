--TEST--
Native component inlines a checked scalar leaf after return copies
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
function w14_scalar_inline_leaf(int $value): int
{
    return $value + 1;
}

function w14_scalar_inline_root(int $count): int
{
    $value = 0;
    for ($index = 0; $index < $count; $index++) {
        $value = w14_scalar_inline_leaf($value);
    }
    return $value;
}
PHP,
    'w14-effect-closed-scalar-inline.php',
    [2000],
    [
        'wave' => 11,
        'function' => 'w14_scalar_inline_root',
        'repeat' => 20,
    ],
);

$execution = $result['execution'];
$performance = $execution['performance'];
printf(
    "%s return=%d runs=%d direct=%d inline=%d typed=%d frame_bytes=%d "
    . "vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $execution['return_value'],
    $execution['executions'],
    $performance['direct_call_sites'],
    $performance['direct_leaf_scalar_sites'],
    $performance['direct_typed_body_sites'],
    $performance['direct_call_frame_bytes'],
    $execution['vm_handler_calls'],
    $execution['execute_ex_calls'],
    $execution['opline_handler_calls'],
);
?>
--EXPECT--
accepted return=2000 runs=20 direct=1 inline=1 typed=0 frame_bytes=112 vm=0 execute_ex=0 handler=0
