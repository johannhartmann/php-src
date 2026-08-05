--TEST--
Native W11 preserves coerced return facts across finally blocks
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
function w11p_verify_return_coercion(): int
{
    $value = 1.5;
    try {
        return $value;
    } finally {
        var_dump($value);
    }
}
PHP,
    'w11p-verify-return-coercion.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_verify_return_coercion',
        'repeat' => 1,
    ],
);
printf(
    "%s return=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECTF--
Deprecated: Implicit conversion from float 1.5 to int loses precision in w11p-verify-return-coercion.php on line 6
float(1.5)
accepted return=1 vm=0 execute_ex=0 handler=0 active=0
