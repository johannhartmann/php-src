--TEST--
Native direct internal calls preserve first-class callable conversion
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
function w11p_internal_callable_convert(): array
{
    $length = strlen(...);
    $slice = substr(...);
    return [$length('test'), $slice('native', 1)];
}
PHP,
    'w11p-internal-callable-convert.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_internal_callable_convert',
        'repeat' => 10,
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
--EXPECT--
accepted return=[4,"ative"] vm=0 execute_ex=0 handler=0 active=0
