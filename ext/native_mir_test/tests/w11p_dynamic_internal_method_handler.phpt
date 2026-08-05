--TEST--
Native dynamic method calls honor internal object method handlers
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
function w11p_dynamic_internal_method_handler(): int
{
    $reflection = new ReflectionClass(SplFileObject::class);
    $object = $reflection->newInstanceWithoutConstructor();
    $callback = [$object, '__debugInfo'];

    try {
        $callback();
    } catch (Error) {
        return 42;
    }
    return 0;
}
PHP,
    'w11p-dynamic-internal-method-handler.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_dynamic_internal_method_handler',
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
accepted return=42 runs=20 vm=0 active=0
