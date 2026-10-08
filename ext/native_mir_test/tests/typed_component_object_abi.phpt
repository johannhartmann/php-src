--TEST--
Native component transports declared objects through the typed TPDE ABI
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
function native_object_leaf(object $value): object
{
    return $value;
}

function native_object_root(object $value): object
{
    return native_object_leaf($value);
}
PHP;

$object = (object) ['id' => 7];
$result = native_mir_test_compile_execute(
    $source,
    'typed-component-object-abi.php',
    [$object],
    [
        'function' => 'native_object_root',
        'repeat' => 20,
    ],
);
$execution = $result['execution'];
$performance = $execution['performance'];
printf(
    "%s id=%d same=%s class=%s runs=%d codeunits=%d components=%d "
    . "direct=%d typed=%d frame_bytes=%d helpers=%d "
    . "vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $execution['return_value']->id,
    $execution['return_value'] === $object ? 'yes' : 'no',
    $execution['return_value']::class,
    $execution['executions'],
    $execution['native_codeunits'],
    $execution['native_components'],
    $performance['direct_call_sites'],
    $performance['direct_typed_body_sites'],
    $performance['direct_call_frame_bytes'],
    $performance['inner_call_runtime_helper_calls'],
    $execution['vm_handler_calls'],
    $execution['execute_ex_calls'],
    $execution['opline_handler_calls'],
);
?>
--EXPECT--
accepted id=7 same=yes class=stdClass runs=20 codeunits=2 components=1 direct=1 typed=2 frame_bytes=0 helpers=0 vm=0 execute_ex=0 handler=0
