--TEST--
Native direct user calls preserve boxed boolean arguments across temporary slot reuse
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
function native_direct_user_boxed_boolean_format($value): string
{
    return json_encode($value);
}

function native_direct_user_boxed_boolean_slot_reuse($left, $right): bool
{
    echo native_direct_user_boxed_boolean_format($left), '/',
        native_direct_user_boxed_boolean_format($left == $right), "\n";
    return true;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'direct-user-boxed-boolean-slot-reuse.php',
    [42, '000042'],
    [
        'function' => 'native_direct_user_boxed_boolean_slot_reuse',
    ],
);
printf(
    "%s return=%s codeunits=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['native_codeunits'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
42/true
accepted return=true codeunits=2 vm=0 execute_ex=0 handler=0 active=0
