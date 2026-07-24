--TEST--
Native baseline passes existing CV references through generated user-call frames
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
function w11p_increment_reference(&$value)
{
    return ++$value;
}

function w11p_reference_root()
{
    $value = 39;
    $alias =& $value;
    return w11p_increment_reference($value)
        + w11p_increment_reference($alias)
        + $value;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-inline-by-reference-user-calls.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_reference_root',
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
accepted return=122 runs=10 codeunits=2 vm=0 execute_ex=0 handler=0 active=0
