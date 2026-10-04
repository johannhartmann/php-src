--TEST--
Native baseline enters monomorphic static user methods with exact called scope
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
final class NativeDirectStaticExact
{
    public static function leaf(int $value): int
    {
        return $value + 2;
    }
}

class NativeDirectStaticBase
{
    protected static int $value = 40;

    final public static function leaf(int $value): int
    {
        return static::$value + $value;
    }
}

final class NativeDirectStaticChild extends NativeDirectStaticBase
{
    protected static int $value = 41;

    public static function run(): int
    {
        return parent::leaf(1) + self::leaf(1) + static::leaf(1);
    }
}

function native_direct_static_user_methods(): int
{
    return NativeDirectStaticExact::leaf(40)
        + NativeDirectStaticChild::run();
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'direct-static-user-methods.php',
    [],
    [
        'function' => 'native_direct_static_user_methods',
        'repeat' => 10,
    ],
);
printf(
    "%s return=%d runs=%d codeunits=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['native_codeunits'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=168 runs=10 codeunits=4 vm=0 execute_ex=0 handler=0 active=0
