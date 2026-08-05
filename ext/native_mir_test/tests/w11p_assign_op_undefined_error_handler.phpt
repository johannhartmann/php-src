--TEST--
Native assign-op materializes undefined CVs after reentrant warnings
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
function w11p_assign_op_undefined_error_handler(): int
{
    global $w11pAssignOpValue;

    unset($w11pAssignOpValue);
    set_error_handler(static function (): void {
        unset($GLOBALS['w11pAssignOpValue']);
    });
    $w11pAssignOpValue -= 1;
    restore_error_handler();

    return $w11pAssignOpValue;
}
PHP,
    'w11p-assign-op-undefined-error-handler.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_assign_op_undefined_error_handler',
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
accepted return=-1 runs=20 vm=0 execute_ex=0 handler=0 active=0
