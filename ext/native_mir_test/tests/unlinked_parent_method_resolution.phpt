--TEST--
Native parent calls bind after class linking across inheritance and traits
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
class NativeUnlinkedGrandparent
{
    public static function inherited(): string
    {
        return 'inherited:' . static::class;
    }

    public static function replacedByTrait(): string
    {
        return 'grandparent:' . static::class;
    }
}

trait NativeUnlinkedParentMethods
{
    public static function replacedByTrait(): string
    {
        return 'parent-trait:' . static::class;
    }
}

class NativeUnlinkedParent extends NativeUnlinkedGrandparent
{
    use NativeUnlinkedParentMethods;

    protected static function local(): string
    {
        return 'local:' . static::class;
    }
}

trait NativeUnlinkedChildCalls
{
    public static function callParentFromTrait(): string
    {
        return parent::replacedByTrait();
    }

    public static function callSelfFromTrait(): string
    {
        return self::collision();
    }

    public static function callStaticFromTrait(): string
    {
        return static::collision();
    }

    public static function collision(): string
    {
        return 'trait-collision:' . static::class;
    }
}

class NativeUnlinkedChild extends NativeUnlinkedParent
{
    use NativeUnlinkedChildCalls;

    public static function collision(): string
    {
        return 'child-collision:' . static::class;
    }

    public static function root(): array
    {
        return [
            parent::inherited(),
            parent::replacedByTrait(),
            parent::local(),
            self::callParentFromTrait(),
            self::callSelfFromTrait(),
            self::callStaticFromTrait(),
        ];
    }
}

final class NativeUnlinkedGrandchild extends NativeUnlinkedChild
{
    public static function collision(): string
    {
        return 'grandchild-collision:' . static::class;
    }
}

function native_unlinked_parent_method_resolution(): array
{
    return NativeUnlinkedGrandchild::root();
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'unlinked-parent-method-resolution.php',
    [],
    [
        'function' => 'native_unlinked_parent_method_resolution',
        'repeat' => 10,
    ],
);
printf(
    "%s return=%s runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value'] ?? null),
    $result['execution']['executions'] ?? -1,
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
    $result['execution']['entry_active_calls'] ?? -1,
);
?>
--EXPECT--
accepted return=["inherited:NativeUnlinkedGrandchild","parent-trait:NativeUnlinkedGrandchild","local:NativeUnlinkedGrandchild","parent-trait:NativeUnlinkedGrandchild","child-collision:NativeUnlinkedGrandchild","grandchild-collision:NativeUnlinkedGrandchild"] runs=10 vm=0 execute_ex=0 handler=0 active=0
