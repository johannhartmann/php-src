--TEST--
Native baseline keeps temporary-heavy user callees on generated frames
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
function w11p_temporary_leaf($value)
{
    return (($value + 3) * 2) - 4;
}

function w11p_temporary_root()
{
    $sum = 0;
    for ($index = 0; $index < 100; $index++) {
        $sum += w11p_temporary_leaf($index);
    }
    return $sum;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-inline-temporary-user-frames.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_temporary_root',
        'repeat' => 10,
    ],
);
printf(
    "%s return=%d runs=%d codeunits=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['native_codeunits'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=10100 runs=10 codeunits=2 vm=0 execute_ex=0 handler=0 active=0
