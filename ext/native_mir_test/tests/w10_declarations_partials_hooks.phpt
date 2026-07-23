--TEST--
Native MIR W10 executes declarations, partial applications, parent hooks, and late argument modes
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
class W10DeclarationBase {
    public function value(): int { return 39; }
}
class W10DynamicArgument {
    public int $value = 40;
}
class W10PartialMethod {
    public function add(int $a, int $b): int { return $a + $b; }
}
class W10ParentHook {
    protected int $backing = 0;
    public int $value {
        get { return $this->backing; }
        set { $this->backing = $value; }
    }
}
class W10ChildHook extends W10ParentHook {
    public int $value {
        get { return parent::$value::get() + 1; }
        set { parent::$value::set($value + 1); }
    }
}

function w10_declared_target(): int { return 1; }
function w10_partial_target(int $a, int $b, int ...$rest): int {
    return $a + $b + $rest[0];
}
function w10_named_partial_target(int $a, int $b): int {
    return $a + $b;
}
function w10_dynamic_ref_target(int &$value): int {
    $value += 2;
    return $value;
}
function w10_dynamic_value_target(int $value): int {
    return $value + 2;
}

function w10_runtime_declarations(): int {
    if (true) {
        function w10_runtime_function(): int { return 1; }
        class W10RuntimeClass extends W10DeclarationBase {
            public function value(): int { return parent::value() + 1; }
        }
    }
    return (new W10RuntimeClass())->value()
        + w10_runtime_function()
        + w10_declared_target();
}
function w10_partial_application(): int {
    $partial = w10_partial_target(?, b: 20, ...);
    return $partial(19, 3);
}
function w10_named_partial_application(): int {
    $partial = w10_named_partial_target(a: ?, b: 40);
    return $partial(a: 2);
}
function w10_method_partial_application(): int {
    $partial = (new W10PartialMethod())->add(?, 2);
    return $partial(40);
}
function w10_parent_property_hook(): int {
    $object = new W10ChildHook();
    $object->value = 40;
    return $object->value;
}
function w10_dynamic_ref_argument(): int {
    $callable = 'w10_dynamic_ref_target';
    $object = new W10DynamicArgument();
    return $callable($object->value);
}
function w10_dynamic_named_value_argument(): int {
    $callable = 'w10_dynamic_value_target';
    $object = new W10DynamicArgument();
    return $callable(value: $object->value);
}
PHP;

foreach ([
    'w10_runtime_declarations',
    'w10_partial_application',
    'w10_named_partial_application',
    'w10_method_partial_application',
    'w10_parent_property_hook',
    'w10_dynamic_ref_argument',
    'w10_dynamic_named_value_argument',
] as $function) {
    $result = native_mir_test_compile_execute(
        $source,
        'w10-declarations-partials-hooks.php',
        [],
        ['wave' => 10, 'function' => $function],
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
w10_runtime_declarations accepted return=42 vm=0 execute_ex=0 handler=0
w10_partial_application accepted return=42 vm=0 execute_ex=0 handler=0
w10_named_partial_application accepted return=42 vm=0 execute_ex=0 handler=0
w10_method_partial_application accepted return=42 vm=0 execute_ex=0 handler=0
w10_parent_property_hook accepted return=42 vm=0 execute_ex=0 handler=0
w10_dynamic_ref_argument accepted return=42 vm=0 execute_ex=0 handler=0
w10_dynamic_named_value_argument accepted return=42 vm=0 execute_ex=0 handler=0
