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
function native_user_callable_target(int $value): int
{
    return $value + 1;
}
function native_user_callable_convert(): int
{
    $callable = native_user_callable_target(...);
    $callable(1);
    return $callable(41);
}
PHP,
    'user-callable-convert.php',
    [],
    [
        'function' => 'native_user_callable_convert',
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
Warning: The return value of function native_user_callable_target() should either be used or intentionally ignored by casting it as (void) in user-callable-convert.php on line 10
accepted return=42 vm=0 execute_ex=0 handler=0 active=0
