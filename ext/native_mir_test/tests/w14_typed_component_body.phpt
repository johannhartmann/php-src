--TEST--
Native component calls a control-flow leaf through the typed TPDE body ABI
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
function w14_typed_leaf(int $value): int
{
    if ($value < 0) {
        return 0;
    }
    return 42;
}

function w14_typed_root(int $value): int
{
    return w14_typed_leaf($value) + w14_typed_leaf(-1);
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w14-typed-component-body.php',
    [41],
    [
        'wave' => 11,
        'function' => 'w14_typed_root',
        'repeat' => 20,
    ],
);
$execution = $result['execution'];
$performance = $execution['performance'];
printf(
	"%s return=%d runs=%d codeunits=%d components=%d direct=%d typed=%d inline=%d frame_bytes=%d unwind=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $execution['return_value'],
    $execution['executions'],
    $execution['native_codeunits'],
    $execution['native_components'],
    $performance['direct_call_sites'],
    $performance['direct_typed_body_sites'],
	$performance['direct_leaf_scalar_sites'],
	$performance['direct_call_frame_bytes'],
	$execution['unwind_registered'],
	$execution['vm_handler_calls'],
    $execution['execute_ex_calls'],
    $execution['opline_handler_calls'],
);
?>
--EXPECT--
accepted return=42 runs=20 codeunits=2 components=1 direct=2 typed=2 inline=0 frame_bytes=0 unwind=0 vm=0 execute_ex=0 handler=0
