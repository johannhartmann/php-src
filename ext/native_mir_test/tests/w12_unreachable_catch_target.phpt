--TEST--
Native catch dispatch ignores unreachable protected-region handlers
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
function w12_unreachable_catch_target(): string
{
    try {
    } catch (RuntimeException $exception) {
        return 'unreachable';
    }

    try {
        throw new Exception('caught');
    } catch (RuntimeException $exception) {
        return 'wrong';
    } catch (Exception $exception) {
        return $exception->getMessage();
    }
}
PHP,
    'w12-unreachable-catch-target.php',
    [],
    [
        'wave' => 11,
        'function' => 'w12_unreachable_catch_target',
        'repeat' => 20,
    ],
);

printf(
    "%s return=%s runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
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
accepted return=caught runs=20 vm=0 execute_ex=0 handler=0 active=0
