--TEST--
Native baseline inherits late-static called scope in generated call frames
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
class NativeInlineCalledScopeBase
{
    final public static function leaf()
    {
        return static::class;
    }
}

final class NativeInlineCalledScopeChild extends NativeInlineCalledScopeBase
{
    public static function root()
    {
        return static::leaf();
    }
}

function native_inline_inherited_called_scope()
{
    return NativeInlineCalledScopeChild::root();
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'inline-inherited-called-scope.php',
    [],
    [
        'function' => 'native_inline_inherited_called_scope',
        'repeat' => 10,
    ],
);
printf(
    "%s return=%s runs=%d codeunits=%d vm=%d execute_ex=%d handler=%d active=%d\n",
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
accepted return=NativeInlineCalledScopeChild runs=10 codeunits=3 vm=0 execute_ex=0 handler=0 active=0
