--TEST--
Native dynamic variable names survive error-handler operand clobbering
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
function dynamic_name_error_handler_lifetime_root(): string
{
    $name = strrev('foo');
    set_error_handler(function () use (&$name): void {
        $name = new stdClass();
    });
    $$name++;
    restore_error_handler();
    return get_debug_type($name);
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-dynamic-name-error-handler-lifetime.php',
    [],
    [
        'wave' => 11,
        'function' => 'dynamic_name_error_handler_lifetime_root',
        'repeat' => 20,
    ],
);
printf(
    "%s return=%s runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return="stdClass" runs=20 vm=0 execute_ex=0 handler=0 active=0
