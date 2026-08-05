--TEST--
Native boxed property reads feed their continuation consumer in registers
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
final class RegisterPropertyBox
{
    public int $value = 7;
}

function register_property_result(int $iterations): int
{
    $box = new RegisterPropertyBox();
    $value = 0;
    for ($index = 0; $index < $iterations; $index++) {
        $value = $box->value;
    }
    return $value;
}
PHP,
    'w11p-inline-object-property-register-result.php',
    [50],
    [
        'wave' => 11,
        'function' => 'register_property_result',
        'repeat' => 10,
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
--EXPECT--
accepted return=7 vm=0 execute_ex=0 handler=0 active=0
