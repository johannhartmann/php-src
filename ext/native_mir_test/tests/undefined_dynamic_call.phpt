--TEST--
Native dynamic calls warn for undefined CVs before reporting null callability
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
function native_undefined_dynamic_call(): string
{
    try {
        $missing();
    } catch (Error $error) {
        return $error->getMessage();
    }
}
PHP,
    'undefined-dynamic-call.php',
    [],
    [
        'function' => 'native_undefined_dynamic_call',
    ],
);
printf(
    "%s return=%s vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);

$result = native_mir_test_compile_execute(
    <<<'PHP'
<?php
function native_undefined_dynamic_call_error_handler(
    int $severity,
    string $message,
): never {
    throw new Exception($message);
}

function native_undefined_dynamic_call_intercepted(): string
{
    set_error_handler('native_undefined_dynamic_call_error_handler');
    try {
        $missing();
    } catch (Throwable $error) {
        restore_error_handler();
        return $error::class . ':' . $error->getMessage();
    }
}
PHP,
    'undefined-dynamic-call-intercepted.php',
    [],
    [
        'function' => 'native_undefined_dynamic_call_intercepted',
    ],
);
printf(
    "%s return=%s vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECTF--
Warning: Undefined variable $missing in %s on line %d
accepted return="Value of type null is not callable" vm=0 execute_ex=0 handler=0 active=0
accepted return="Exception:Undefined variable $missing" vm=0 execute_ex=0 handler=0 active=0
