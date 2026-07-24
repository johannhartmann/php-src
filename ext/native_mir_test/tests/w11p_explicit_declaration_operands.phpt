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
class W11PDeclarationBase {
    public function value(): int { return 40; }
}
class W11PDelayedDeclaration extends W11PDeclarationBase {
    public function value(): int { return parent::value() + 2; }
}
function w11p_explicit_anonymous_declaration(): int {
    $object = new class extends W11PDeclarationBase {
        public function value(): int { return parent::value() + 2; }
    };
    return $object->value();
}
function w11p_explicit_closure_bindings(): int {
    $base = 39;
    $offset = 1;
    $closure = function () use ($base, &$offset): int {
        static $calls = 0;
        $offset++;
        return $base + $offset + ++$calls;
    };
    return $closure();
}
function w11p_explicit_runtime_declarations(): int {
    if (true) {
        function w11p_explicit_nested_function(): int { return 20; }
        class W11PExplicitNestedClass extends W11PDeclarationBase {
            public function value(): int {
                return parent::value() - 18;
            }
        }
    }
    return w11p_explicit_nested_function()
        + (new W11PExplicitNestedClass())->value();
}
function w11p_explicit_delayed_declaration(): int {
    return (new W11PDelayedDeclaration())->value();
}
PHP;

foreach ([
    'w11p_explicit_anonymous_declaration',
    'w11p_explicit_closure_bindings',
    'w11p_explicit_runtime_declarations',
    'w11p_explicit_delayed_declaration',
] as $function) {
    $result = native_mir_test_compile_execute(
        $source,
        'w11p-explicit-declaration-operands.php',
        [],
        ['wave' => 11, 'function' => $function],
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
w11p_explicit_anonymous_declaration accepted return=42 vm=0 execute_ex=0 handler=0
w11p_explicit_closure_bindings accepted return=42 vm=0 execute_ex=0 handler=0
w11p_explicit_runtime_declarations accepted return=42 vm=0 execute_ex=0 handler=0
w11p_explicit_delayed_declaration accepted return=42 vm=0 execute_ex=0 handler=0
