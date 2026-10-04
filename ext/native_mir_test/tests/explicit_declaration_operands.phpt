--TEST--
Native declarations and closure bindings consume explicit operands
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
class NativeDeclarationBase {
    public function value(): int { return 40; }
}
class NativeDelayedDeclaration extends NativeDeclarationBase {
    public function value(): int { return parent::value() + 2; }
}
function native_explicit_anonymous_declaration(): int {
    $object = new class extends NativeDeclarationBase {
        public function value(): int { return parent::value() + 2; }
    };
    return $object->value();
}
function native_explicit_closure_bindings(): int {
    $base = 39;
    $offset = 1;
    $closure = function () use ($base, &$offset): int {
        static $calls = 0;
        $offset++;
        return $base + $offset + ++$calls;
    };
    return $closure();
}
function native_explicit_runtime_declarations(): int {
    if (true) {
        function native_explicit_nested_function(): int { return 20; }
        class NativeExplicitNestedClass extends NativeDeclarationBase {
            public function value(): int {
                return parent::value() - 18;
            }
        }
    }
    return native_explicit_nested_function()
        + (new NativeExplicitNestedClass())->value();
}
function native_explicit_delayed_declaration(): int {
    return (new NativeDelayedDeclaration())->value();
}
PHP;

foreach ([
    'native_explicit_anonymous_declaration',
    'native_explicit_closure_bindings',
    'native_explicit_runtime_declarations',
    'native_explicit_delayed_declaration',
] as $function) {
    $result = native_mir_test_compile_execute(
        $source,
        'explicit-declaration-operands.php',
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
native_explicit_anonymous_declaration accepted return=42 vm=0 execute_ex=0 handler=0
native_explicit_closure_bindings accepted return=42 vm=0 execute_ex=0 handler=0
native_explicit_runtime_declarations accepted return=42 vm=0 execute_ex=0 handler=0
native_explicit_delayed_declaration accepted return=42 vm=0 execute_ex=0 handler=0
