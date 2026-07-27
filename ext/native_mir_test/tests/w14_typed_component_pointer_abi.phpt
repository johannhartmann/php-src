--TEST--
Native component transports strings through the typed TPDE pointer ABI
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
function w14_pointer_leaf(string $value): string
{
    return $value;
}

function w14_pointer_root(string $value): string
{
    return w14_pointer_leaf($value);
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w14-typed-component-pointer-abi.php',
    ['register-authoritative'],
    [
        'wave' => 11,
        'function' => 'w14_pointer_root',
        'repeat' => 20,
    ],
);
$execution = $result['execution'];
$performance = $execution['performance'];
printf(
    "%s return=%s runs=%d codeunits=%d components=%d direct=%d typed=%d frame_bytes=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $execution['return_value'],
    $execution['executions'],
    $execution['native_codeunits'],
    $execution['native_components'],
    $performance['direct_call_sites'],
    $performance['direct_typed_body_sites'],
    $performance['direct_call_frame_bytes'],
    $execution['vm_handler_calls'],
    $execution['execute_ex_calls'],
    $execution['opline_handler_calls'],
);
?>
--EXPECT--
accepted return=register-authoritative runs=20 codeunits=2 components=1 direct=1 typed=2 frame_bytes=0 vm=0 execute_ex=0 handler=0
