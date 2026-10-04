--TEST--
Native internal callbacks normalize magic-call trampolines before reentry
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
final class NativeInternalCallbackMagicTrampoline
{
    public function __call(string $name, array $arguments): string
    {
        return $name . ':' . $arguments[0];
    }
}

function native_internal_callback_magic_trampoline(): array
{
    $object = new NativeInternalCallbackMagicTrampoline();
    return array_map([$object, 'missing'], ['a', 'b']);
}
PHP,
    'internal-callback-magic-trampoline.php',
    [],
    [
        'function' => 'native_internal_callback_magic_trampoline',
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
accepted return=["missing:a","missing:b"] runs=20 vm=0 execute_ex=0 handler=0 active=0
