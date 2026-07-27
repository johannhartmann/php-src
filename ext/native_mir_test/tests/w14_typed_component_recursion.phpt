--TEST--
Native mutually recursive scalar calls stay inside the typed TPDE body ABI
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
function w14_mutual_a(bool $again): int
{
    if ($again === true) {
        return w14_mutual_b(false);
    }
    return 41;
}

function w14_mutual_b(bool $again): int
{
    if ($again === true) {
        return w14_mutual_a(false);
    }
    return 42;
}

function w14_mutual_root(bool $again): int
{
    return w14_mutual_a($again);
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w14-typed-component-recursion.php',
    [true],
    [
        'wave' => 11,
        'function' => 'w14_mutual_root',
        'repeat' => 20,
    ],
);
$execution = $result['execution'];
$performance = $execution['performance'];
printf(
    "%s return=%d runs=%d codeunits=%d components=%d direct=%d typed=%d frame_bytes=%d vm=%d execute_ex=%d handler=%d\n",
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
accepted return=42 runs=20 codeunits=3 components=1 direct=3 typed=6 frame_bytes=0 vm=0 execute_ex=0 handler=0
