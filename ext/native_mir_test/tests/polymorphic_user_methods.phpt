--TEST--
Native baseline keeps overrideable user methods on the polymorphic call path
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
class NativePolymorphicBase
{
    public function value(): int
    {
        return 1;
    }
}

class NativePolymorphicChild extends NativePolymorphicBase
{
    public function value(): int
    {
        return 42;
    }
}

$source = <<<'PHP'
<?php
function native_polymorphic_method(NativePolymorphicBase $receiver): int
{
    return $receiver->value();
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'polymorphic-user-methods.php',
    [new NativePolymorphicChild()],
    [
        'function' => 'native_polymorphic_method',
        'repeat' => 10,
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
accepted return=42 runs=10 vm=0 execute_ex=0 handler=0 active=0
