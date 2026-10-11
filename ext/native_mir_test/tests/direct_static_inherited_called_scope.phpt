--TEST--
Native direct self::, parent:: and static:: calls pass the inherited called scope from static and instance callers
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
class NativeInheritScopeA
{
    public const VALUE = 1;

    final public static function leaf(int $value): int
    {
        return static::VALUE * 1000 + $value;
    }
}

class NativeInheritScopeB extends NativeInheritScopeA
{
    public const VALUE = 2;

    public function fromInstance(int $value): int
    {
        return self::leaf($value) + parent::leaf($value) * 10
            + static::leaf($value) * 100;
    }

    public static function fromStatic(int $value): int
    {
        return self::leaf($value) + parent::leaf($value) * 10
            + static::leaf($value) * 100;
    }
}

final class NativeInheritScopeC extends NativeInheritScopeB
{
    public const VALUE = 3;
}

function native_inherit_called_scope(): int
{
    $b = new NativeInheritScopeB();
    $c = new NativeInheritScopeC();
    return $b->fromInstance(1) + $c->fromInstance(2) * 3
        + NativeInheritScopeB::fromStatic(3) * 5
        + NativeInheritScopeC::fromStatic(4) * 7;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'direct-static-inherited-called-scope.php',
    [],
    [
        'function' => 'native_inherit_called_scope',
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
accepted return=4667550 runs=10 vm=0 execute_ex=0 handler=0 active=0
