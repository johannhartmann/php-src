--TEST--
Native internal calls wrap no-ref temporaries in the call frame
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
function w11p_internal_no_ref_temporary(): int
{
    return next(array_values([1, 2]));
}
PHP,
    'w11p-internal-no-ref-temporary.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_internal_no_ref_temporary',
    ],
);
printf(
    "%s return=%s vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECTF--
Notice: Only variables should be passed by reference in w11p-internal-no-ref-temporary.php on line %d
accepted return=2 vm=0 execute_ex=0 handler=0 active=0
