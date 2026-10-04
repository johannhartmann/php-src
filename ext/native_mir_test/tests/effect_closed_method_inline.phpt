--TEST--
Native component inlines only effect-closed methods with borrowed receivers
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
final class NativeEffectClosedMethod
{
    private int $base = 40;

    public function step(int $value): int
    {
        return $value + 1;
    }

    public function receiverValue(): int
    {
        return $this->base;
    }
}

function native_borrowed_receiver(int $count): int
{
    $object = new NativeEffectClosedMethod();
    $value = 0;
    for ($index = 0; $index < $count; $index++) {
        $value = $object->step($value);
    }
    return $value;
}

function native_used_receiver(): int
{
    $object = new NativeEffectClosedMethod();
    return $object->receiverValue();
}

function native_consumed_receiver(): int
{
    return (new NativeEffectClosedMethod())->step(1);
}
PHP;

foreach (
    [
        ['native_borrowed_receiver', [20]],
        ['native_used_receiver', []],
        ['native_consumed_receiver', []],
    ] as [$function, $arguments]
) {
    $result = native_mir_test_compile_execute(
        $source,
        'effect-closed-method-inline.php',
        $arguments,
        [
            'function' => $function,
            'repeat' => 10,
        ],
    );
    $execution = $result['execution'];
    $performance = $execution['performance'];
    printf(
        "%s %s return=%d direct=%d inline=%d typed=%d vm=%d execute_ex=%d handler=%d\n",
        $function,
        $result['status'],
        $execution['return_value'],
        $performance['direct_call_sites'],
        $performance['direct_leaf_scalar_sites'],
        $performance['direct_typed_body_sites'],
        $execution['vm_handler_calls'],
        $execution['execute_ex_calls'],
        $execution['opline_handler_calls'],
    );
}
?>
--EXPECT--
native_borrowed_receiver accepted return=20 direct=1 inline=1 typed=0 vm=0 execute_ex=0 handler=0
native_used_receiver accepted return=40 direct=1 inline=0 typed=0 vm=0 execute_ex=0 handler=0
native_consumed_receiver accepted return=2 direct=1 inline=0 typed=0 vm=0 execute_ex=0 handler=0
