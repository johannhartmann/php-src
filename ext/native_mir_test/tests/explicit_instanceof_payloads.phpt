--TEST--
Native instanceof consumes explicit operands and class-fetch payloads
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
class NativeInstanceofBase {}
final class NativeInstanceofChild extends NativeInstanceofBase {
    public static function againstSelf(object $value): bool {
        return $value instanceof self;
    }
    public static function againstParent(object $value): bool {
        return $value instanceof parent;
    }
}
function native_instanceof_literal(): bool {
    return new NativeInstanceofChild() instanceof NativeInstanceofBase;
}
function native_instanceof_dynamic(): bool {
    $class = NativeInstanceofBase::class;
    return new NativeInstanceofChild() instanceof $class;
}
function native_instanceof_self(): bool {
    return NativeInstanceofChild::againstSelf(new NativeInstanceofChild());
}
function native_instanceof_parent(): bool {
    return NativeInstanceofChild::againstParent(new NativeInstanceofChild());
}
PHP;

foreach ([
    'native_instanceof_literal',
    'native_instanceof_dynamic',
    'native_instanceof_self',
    'native_instanceof_parent',
] as $function) {
    $result = native_mir_test_compile_execute(
        $source,
        'explicit-instanceof-payloads.php',
        [],
        ['function' => $function],
    );
    printf(
        "%s %s return=%s vm=%d execute_ex=%d handler=%d\n",
        $function,
        $result['status'],
        json_encode($result['execution']['return_value'] ?? null),
        $result['execution']['vm_handler_calls'] ?? -1,
        $result['execution']['execute_ex_calls'] ?? -1,
        $result['execution']['opline_handler_calls'] ?? -1,
    );
}
?>
--EXPECT--
native_instanceof_literal accepted return=true vm=0 execute_ex=0 handler=0
native_instanceof_dynamic accepted return=true vm=0 execute_ex=0 handler=0
native_instanceof_self accepted return=true vm=0 execute_ex=0 handler=0
native_instanceof_parent accepted return=true vm=0 execute_ex=0 handler=0
