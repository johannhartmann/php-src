--TEST--
Native MIR executes declarations, partial applications, parent hooks, and late argument modes
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
    public function value(): int { return 39; }
}
class NativeDynamicArgument {
    public int $value = 40;
}
class NativePartialMethod {
    public function add(int $a, int $b): int { return $a + $b; }
}
class NativeParentHook {
    protected int $backing = 0;
    public int $value {
        get { return $this->backing; }
        set { $this->backing = $value; }
    }
}
class NativeChildHook extends NativeParentHook {
    public int $value {
        get { return parent::$value::get() + 1; }
        set { parent::$value::set($value + 1); }
    }
}

function native_declared_target(): int { return 1; }
function native_partial_target(int $a, int $b, int ...$rest): int {
    return $a + $b + $rest[0];
}
function native_named_partial_target(int $a, int $b): int {
    return $a + $b;
}
function native_dynamic_ref_target(int &$value): int {
    $value += 2;
    return $value;
}
function native_dynamic_value_target(int $value): int {
    return $value + 2;
}

function native_runtime_declarations(): int {
    if (true) {
        function native_runtime_function(): int { return 1; }
        class NativeRuntimeClass extends NativeDeclarationBase {
            public function value(): int { return parent::value() + 1; }
        }
    }
    return (new NativeRuntimeClass())->value()
        + native_runtime_function()
        + native_declared_target();
}
function native_partial_application(): int {
    $partial = native_partial_target(?, b: 20, ...);
    return $partial(19, 3);
}
function native_named_partial_application(): int {
    $partial = native_named_partial_target(a: ?, b: 40);
    return $partial(a: 2);
}
function native_method_partial_application(): int {
    $partial = (new NativePartialMethod())->add(?, 2);
    return $partial(40);
}
function native_parent_property_hook(): int {
    $object = new NativeChildHook();
    $object->value = 40;
    return $object->value;
}
function native_dynamic_ref_argument(): int {
    $callable = 'native_dynamic_ref_target';
    $object = new NativeDynamicArgument();
    return $callable($object->value);
}
function native_dynamic_named_value_argument(): int {
    $callable = 'native_dynamic_value_target';
    $object = new NativeDynamicArgument();
    return $callable(value: $object->value);
}
PHP;

foreach ([
    'native_runtime_declarations',
    'native_partial_application',
    'native_named_partial_application',
    'native_method_partial_application',
    'native_parent_property_hook',
    'native_dynamic_ref_argument',
    'native_dynamic_named_value_argument',
] as $function) {
    $result = native_mir_test_compile_execute(
        $source,
        'declarations-partials-hooks.php',
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
native_runtime_declarations accepted return=42 vm=0 execute_ex=0 handler=0
native_partial_application accepted return=42 vm=0 execute_ex=0 handler=0
native_named_partial_application accepted return=42 vm=0 execute_ex=0 handler=0
native_method_partial_application accepted return=42 vm=0 execute_ex=0 handler=0
native_parent_property_hook accepted return=42 vm=0 execute_ex=0 handler=0
native_dynamic_ref_argument accepted return=42 vm=0 execute_ex=0 handler=0
native_dynamic_named_value_argument accepted return=42 vm=0 execute_ex=0 handler=0
