--TEST--
Native partial applications preserve runtime validation errors
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
function w11p_partial_runtime_errors(): int
{
    $result = 0;
    try {
        property_exists(?);
    } catch (ArgumentCountError $error) {
        $result += 1;
    }
    try {
        func_get_args(?);
    } catch (Error $error) {
        $result += 2;
    }
    $repeat = str_repeat('a', ...);
    try {
        $repeat();
    } catch (ArgumentCountError $error) {
        $result += 4;
    }
    return $result;
}
PHP,
    'w11p-partial-application-runtime-errors.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_partial_runtime_errors',
        'repeat' => 20,
    ],
);
printf(
    "%s return=%d runs=%d vm=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=7 runs=20 vm=0 active=0
