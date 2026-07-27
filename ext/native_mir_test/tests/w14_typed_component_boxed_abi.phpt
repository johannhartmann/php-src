--TEST--
Native component transports refcounted boxed values through the typed TPDE ABI
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
function w14_boxed_leaf($value)
{
    return $value;
}

function w14_boxed_root($value)
{
    return w14_boxed_leaf($value);
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w14-typed-component-boxed-abi.php',
    [['a' => 42]],
    [
        'wave' => 11,
        'function' => 'w14_boxed_root',
        'repeat' => 20,
    ],
);
$execution = $result['execution'];
$performance = $execution['performance'];
printf(
    "%s return=%d count=%d runs=%d codeunits=%d components=%d "
    . "direct=%d typed=%d frame_bytes=%d helpers=%d allocations=%d "
    . "catchers=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $execution['return_value']['a'],
    count($execution['return_value']),
    $execution['executions'],
    $execution['native_codeunits'],
    $execution['native_components'],
    $performance['direct_call_sites'],
    $performance['direct_typed_body_sites'],
    $performance['direct_call_frame_bytes'],
    $performance['inner_call_runtime_helper_calls'],
    $performance['inner_call_heap_allocations'],
    $performance['inner_call_catcher_boundaries'],
    $execution['vm_handler_calls'],
    $execution['execute_ex_calls'],
    $execution['opline_handler_calls'],
);
?>
--EXPECT--
accepted return=42 count=1 runs=20 codeunits=2 components=1 direct=1 typed=2 frame_bytes=0 helpers=0 allocations=0 catchers=0 vm=0 execute_ex=0 handler=0
