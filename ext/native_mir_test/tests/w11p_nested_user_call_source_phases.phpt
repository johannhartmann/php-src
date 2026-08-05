--TEST--
Native nested user calls preserve the inner source argument
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
function w11p_nested_user_call_identity(mixed $value): mixed
{
    return $value;
}

function w11p_nested_user_call_source_phases(mixed $value): mixed
{
    return w11p_nested_user_call_identity(
        w11p_nested_user_call_identity($value)
    );
}
PHP,
    'w11p-nested-user-call-source-phases.php',
    [42],
    [
        'wave' => 11,
        'function' => 'w11p_nested_user_call_source_phases',
        'repeat' => 20,
    ],
);
printf(
    "%s return=%d runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=42 runs=20 vm=0 execute_ex=0 handler=0 active=0
