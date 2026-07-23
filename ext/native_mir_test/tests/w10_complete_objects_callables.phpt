--TEST--
Native MIR W10 executes complete object, method, and callable semantics
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
interface W10CompleteInterface { public function compute(int $value): int; }
trait W10CompleteFirst { public function traitValue(): int { return 20; } }
trait W10CompleteSecond { public function traitValue(): int { return 21; } }
class W10CompleteBase {
    protected int $protected = 10;
    private int $private = 11;
    public static int $staticValue = 40;
    public const ANSWER = 42;
    public function scoped(): int { return $this->private + $this->protected + 21; }
    public static function late(): int { return static::$staticValue + 2; }
    public static function forwarded(int $value): int { return $value + 2; }
    public static function forwardCall(int $value): int {
        return forward_static_call([static::class, 'forwarded'], $value);
    }
}
class W10CompleteObject extends W10CompleteBase implements W10CompleteInterface {
    use W10CompleteFirst, W10CompleteSecond {
        W10CompleteFirst::traitValue insteadof W10CompleteSecond;
        W10CompleteSecond::traitValue as alternateValue;
    }
    public int $typed;
    public array $items = [];
    public function __construct(public int $promoted = 40) { $this->typed = 2; }
    public function compute(int $value): int { return $this->promoted + $value; }
    public function dynamicRef(int &$value, int ...$rest): int {
        $value++;
        return $value + $rest[0];
    }
    public static function staticDynamic(int $value): int { return $value + 2; }
}
class W10CompleteChild extends W10CompleteObject { public static int $staticValue = 40; }
class W10CompleteMagic {
    public function __call(string $name, array $arguments): int {
        return $name === 'answer' ? $arguments[0] + 2 : 0;
    }
    public static function __callStatic(string $name, array $arguments): int {
        return $name === 'answer' ? $arguments[0] + 2 : 0;
    }
    public function __invoke(int $value): int { return $value + 2; }
    public function __toString(): string { return '42'; }
    public function __serialize(): array { return ['value' => 42]; }
    public function __unserialize(array $data): void {}
    public static function __set_state(array $data): object { return new self(); }
    public function __debugInfo(): array { return ['value' => 42]; }
}
class W10CompleteHooks {
    private int $backing = 40;
    public int $value {
        get => $this->backing + 2;
        set { $this->backing = $value; }
    }
}
class W10CompleteReadonly { public function __construct(public readonly int $value) {} }
class W10CompletePrivateConstructor {
    private function __construct() {}
    public static function create(): self { return new self(); }
    public function value(): int { return 42; }
}
class W10CompleteNoConstructor { public int $value = 42; }
class W10CompletePrivateMethods {
    private function hidden(): int { return 0; }
    private static function hiddenStatic(): int { return 0; }
}
class W10CompletePropertyModifiers {
    public private(set) int $asymmetric = 42;
    final public int $finalValue = 42;
}
#[AllowDynamicProperties]
class W10CompleteDynamic {}
class W10CompleteDeprecatedDynamic {}

function w10_complete_property_modes(): int {
    $object = new W10CompleteObject();
    $object->items['value'] = 38;
    $object->items['value'] += 1;
    $object->items['value']++;
    ++$object->items['value'];
    unset($object->items['missing']);
    return isset($object->items['value']) && !empty($object->items['value'])
        ? $object->items['value'] + 1
        : 0;
}
function w10_complete_property_reference(): int {
    $object = new W10CompleteObject();
    $reference =& $object->promoted;
    $reference += 2;
    return $object->promoted;
}
function w10_complete_static_modes(): int {
    W10CompleteObject::$staticValue = 38;
    $reference =& W10CompleteObject::$staticValue;
    $reference++;
    ++W10CompleteObject::$staticValue;
    W10CompleteObject::$staticValue += 2;
    return W10CompleteObject::$staticValue;
}
function w10_complete_scope_constant(): int {
    return (new W10CompleteBase())->scoped() + W10CompleteBase::ANSWER - 42;
}
function w10_complete_trait_alias(): int {
    $object = new W10CompleteObject();
    return $object->traitValue() + $object->alternateValue() + 1;
}
function w10_complete_dynamic_method(): int {
    $object = new W10CompleteChild();
    $method = 'compute';
    return $object->$method(value: 2);
}
function w10_complete_dynamic_static(): int {
    $class = W10CompleteObject::class;
    $method = 'staticDynamic';
    return $class::$method(40);
}
function w10_complete_magic_call(): int {
    $object = new W10CompleteMagic();
    $method = 'answer';
    return $object->$method(40);
}
function w10_complete_magic_static(): int {
    $class = W10CompleteMagic::class;
    $method = 'answer';
    return $class::$method(40);
}
function w10_complete_method_arguments(): int {
    $object = new W10CompleteObject();
    $value = 39;
    $rest = [2];
    return $object->dynamicRef($value, ...$rest);
}
function w10_complete_invoke(): int { return (new W10CompleteMagic())(40); }
function w10_complete_closure_call(): int {
    $object = new W10CompleteObject();
    $closure = function(int $value): int { return $this->promoted + $value; };
    return $closure->call($object, 2);
}
function w10_complete_closure_bind(): int {
    $object = new W10CompleteObject();
    $closure = function(): int { return $this->promoted + 2; };
    return $closure->bindTo($object, W10CompleteObject::class)();
}
function w10_complete_closure_from_callable(): int {
    return Closure::fromCallable([W10CompleteObject::class, 'staticDynamic'])(40);
}
function w10_complete_callback_array(): int {
    return call_user_func_array([W10CompleteObject::class, 'staticDynamic'], [40]);
}
function w10_complete_callable_check(): int {
    $object = new W10CompleteMagic();
    return is_callable($object) && is_callable([$object, 'answer']) ? 42 : 0;
}
function w10_complete_nested_dynamic(): int {
    $target = 'w10_complete_nested_target';
    return $target(40);
}
function w10_complete_nested_target(int $value): int {
    $target = 'w10_complete_nested_leaf';
    return $target($value);
}
function w10_complete_nested_leaf(int $value): int { return $value + 2; }
function w10_complete_recursive_closure(): int {
    $sum = function(int $value) use (&$sum): int {
        return $value === 0 ? 0 : 1 + $sum($value - 1);
    };
    return $sum(42);
}
function w10_complete_hook(): int {
    $object = new W10CompleteHooks();
    $object->value = 40;
    return $object->value;
}
function w10_complete_readonly(): int { return (new W10CompleteReadonly(42))->value; }
function w10_complete_alias(): int {
    class_alias(W10CompleteObject::class, 'W10CompleteAlias');
    $class = 'W10CompleteAlias';
    return (new $class(40))->compute(2);
}
function w10_complete_lazy_ghost(): int {
    $reflection = new ReflectionClass(W10CompleteObject::class);
    $object = $reflection->newLazyGhost(
        function(W10CompleteObject $object): void { $object->__construct(40); }
    );
    return $object->compute(2);
}
function w10_complete_lazy_proxy(): int {
    $reflection = new ReflectionClass(W10CompleteObject::class);
    $object = $reflection->newLazyProxy(
        function(W10CompleteObject $object): W10CompleteObject {
            return new W10CompleteObject(40);
        }
    );
    return $object->compute(2);
}
function w10_complete_weakmap(): int {
    $object = new W10CompleteObject(40);
    $map = new WeakMap();
    $map[$object] = 42;
    return $map[$object];
}
function w10_complete_internal_handlers(): int {
    $object = new ArrayObject(['value' => 40]);
    $object['value'] += 2;
    return $object['value'];
}
function w10_complete_magic_serialization(): int {
    $copy = unserialize(serialize(new W10CompleteMagic()));
    return (string) $copy === '42' ? 42 : 0;
}
function w10_complete_lsb(): int { return W10CompleteChild::late(); }
function w10_complete_dynamic_property(): int {
    $object = new W10CompleteDynamic();
    $object->value = 42;
    return $object->value;
}
function w10_complete_dynamic_property_deprecation(): int {
    $deprecations = 0;
    set_error_handler(
        function(int $severity) use (&$deprecations): bool {
            if ($severity === E_DEPRECATED) {
                $deprecations++;
            }
            return true;
        }
    );
    $object = new W10CompleteDeprecatedDynamic();
    $object->value = 40;
    restore_error_handler();
    return $object->value + $deprecations + 1;
}
function w10_complete_property_modifiers(): int {
    $object = new W10CompletePropertyModifiers();
    try {
        $object->asymmetric = 1;
    } catch (Error $error) {
        return str_contains($error->getMessage(), 'private(set)')
            && $object->finalValue === 42 ? 42 : 0;
    }
    return 0;
}
function w10_complete_object_cast(): int {
    $values = (array) new W10CompleteObject(40);
    return $values['promoted'] + $values['typed'];
}
function w10_complete_debug_info(): int {
    return (new W10CompleteMagic())->__debugInfo()['value'];
}
function w10_complete_missing_constructor(): int {
    return (new W10CompleteNoConstructor())->value;
}
function w10_complete_private_constructor_error(): int {
    try {
        new W10CompletePrivateConstructor();
    } catch (Error $error) {
        return str_contains($error->getMessage(), 'private') ? 42 : 0;
    }
    return 0;
}
function w10_complete_private_constructor(): int {
    return W10CompletePrivateConstructor::create()->value();
}
function w10_complete_forward_static(): int {
    return W10CompleteChild::forwardCall(40);
}
function w10_complete_private_method_error(): int {
    try {
        (new W10CompletePrivateMethods())->hidden();
    } catch (Error $error) {
        return str_contains($error->getMessage(), 'private') ? 42 : 0;
    }
    return 0;
}
function w10_complete_private_static_method_error(): int {
    try {
        W10CompletePrivateMethods::hiddenStatic();
    } catch (Error $error) {
        return str_contains($error->getMessage(), 'private') ? 42 : 0;
    }
    return 0;
}
function w10_complete_invalid_callable_error(): int {
    $callable = [W10CompletePrivateMethods::class, 'missing'];
    try {
        $callable();
    } catch (Error $error) {
        return 42;
    }
    return 0;
}
function w10_complete_throw_finally(): int {
    try {
        throw new RuntimeException('native');
    } catch (RuntimeException $error) {
        $value = $error->getMessage() === 'native' ? 41 : 0;
    } finally {
        $value++;
    }
    return $value;
}
PHP;

$functions = [
    'w10_complete_property_modes',
    'w10_complete_property_reference',
    'w10_complete_static_modes',
    'w10_complete_scope_constant',
    'w10_complete_trait_alias',
    'w10_complete_dynamic_method',
    'w10_complete_dynamic_static',
    'w10_complete_magic_call',
    'w10_complete_magic_static',
    'w10_complete_method_arguments',
    'w10_complete_invoke',
    'w10_complete_closure_call',
    'w10_complete_closure_bind',
    'w10_complete_closure_from_callable',
    'w10_complete_callback_array',
    'w10_complete_callable_check',
    'w10_complete_nested_dynamic',
    'w10_complete_recursive_closure',
    'w10_complete_hook',
    'w10_complete_readonly',
    'w10_complete_alias',
    'w10_complete_lazy_ghost',
    'w10_complete_lazy_proxy',
    'w10_complete_weakmap',
    'w10_complete_internal_handlers',
    'w10_complete_magic_serialization',
    'w10_complete_lsb',
    'w10_complete_dynamic_property',
    'w10_complete_dynamic_property_deprecation',
    'w10_complete_property_modifiers',
    'w10_complete_object_cast',
    'w10_complete_debug_info',
    'w10_complete_missing_constructor',
    'w10_complete_private_constructor_error',
    'w10_complete_private_constructor',
    'w10_complete_forward_static',
    'w10_complete_private_method_error',
    'w10_complete_private_static_method_error',
    'w10_complete_invalid_callable_error',
    'w10_complete_throw_finally',
];
foreach ($functions as $function) {
    $result = native_mir_test_compile_execute(
        $source,
        'w10-complete-objects-callables.php',
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
w10_complete_property_modes accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_property_reference accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_static_modes accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_scope_constant accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_trait_alias accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_dynamic_method accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_dynamic_static accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_magic_call accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_magic_static accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_method_arguments accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_invoke accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_closure_call accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_closure_bind accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_closure_from_callable accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_callback_array accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_callable_check accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_nested_dynamic accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_recursive_closure accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_hook accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_readonly accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_alias accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_lazy_ghost accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_lazy_proxy accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_weakmap accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_internal_handlers accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_magic_serialization accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_lsb accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_dynamic_property accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_dynamic_property_deprecation accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_property_modifiers accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_object_cast accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_debug_info accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_missing_constructor accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_private_constructor_error accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_private_constructor accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_forward_static accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_private_method_error accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_private_static_method_error accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_invalid_callable_error accepted return=42 vm=0 execute_ex=0 handler=0
w10_complete_throw_finally accepted return=42 vm=0 execute_ex=0 handler=0
