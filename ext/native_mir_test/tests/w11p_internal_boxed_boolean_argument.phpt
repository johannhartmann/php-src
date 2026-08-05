--TEST--
Native direct internal calls preserve boxed boolean arguments
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
function w11p_internal_boxed_boolean_argument(int $value): bool
{
    var_dump($value < 10);
    return $value < 10;
}
PHP,
    'w11p-internal-boxed-boolean-argument.php',
    [3],
    [
        'wave' => 11,
        'function' => 'w11p_internal_boxed_boolean_argument',
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
bool(true)
accepted return=true vm=0 execute_ex=0 handler=0 active=0
