--TEST--
Native direct user calls preserve first-class callable conversion
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
#[\NoDiscard]
function w11p_user_callable_target(int $value): int
{
    return $value + 1;
}
function w11p_user_callable_convert(): int
{
    $callable = w11p_user_callable_target(...);
    $callable(1);
    return $callable(41);
}
PHP,
    'w11p-user-callable-convert.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_user_callable_convert',
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
Warning: The return value of function w11p_user_callable_target() should either be used or intentionally ignored by casting it as (void) in w11p-user-callable-convert.php on line 10
accepted return=42 vm=0 execute_ex=0 handler=0 active=0
