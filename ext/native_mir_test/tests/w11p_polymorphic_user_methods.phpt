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
class W11PPolymorphicBase
{
    public function value(): int
    {
        return 1;
    }
}

class W11PPolymorphicChild extends W11PPolymorphicBase
{
    public function value(): int
    {
        return 42;
    }
}

$source = <<<'PHP'
<?php
function w11p_polymorphic_method(W11PPolymorphicBase $receiver): int
{
    return $receiver->value();
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-polymorphic-user-methods.php',
    [new W11PPolymorphicChild()],
    [
        'wave' => 11,
        'function' => 'w11p_polymorphic_method',
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
