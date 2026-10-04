--TEST--
Native code executes complete object, method, and callable semantics
--EXTENSIONS--
native_mir_test
--FILE--
<?php
interface NativeCompleteInterface { public function compute(int $value): int; }
trait NativeCompleteFirst { public function traitValue(): int { return 20; } }
trait NativeCompleteSecond { public function traitValue(): int { return 21; } }
class NativeCompleteBase {
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
class NativeCompleteObject extends NativeCompleteBase implements NativeCompleteInterface {
    use NativeCompleteFirst, NativeCompleteSecond {
        NativeCompleteFirst::traitValue insteadof NativeCompleteSecond;
        NativeCompleteSecond::traitValue as alternateValue;
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
class NativeCompleteChild extends NativeCompleteObject { public static int $staticValue = 40; }
class NativeCompleteMagic {
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
class NativeCompleteHooks {
    private int $backing = 40;
    public int $value {
        get => $this->backing + 2;
        set { $this->backing = $value; }
    }
}
class NativeCompleteReadonly { public function __construct(public readonly int $value) {} }
class NativeCompletePrivateConstructor {
    private function __construct() {}
    public static function create(): self { return new self(); }
    public function value(): int { return 42; }
}
class NativeCompleteNoConstructor { public int $value = 42; }
class NativeCompletePrivateMethods {
    private function hidden(): int { return 0; }
    private static function hiddenStatic(): int { return 0; }
}
class NativeCompletePropertyModifiers {
    public private(set) int $asymmetric = 42;
    final public int $finalValue = 42;
}
#[AllowDynamicProperties]
class NativeCompleteDynamic {}
class NativeCompleteDeprecatedDynamic {}

function native_complete_property_modes(): int {
    $object = new NativeCompleteObject();
    $object->items['value'] = 38;
    $object->items['value'] += 1;
    $object->items['value']++;
    ++$object->items['value'];
    unset($object->items['missing']);
    return isset($object->items['value']) && !empty($object->items['value'])
        ? $object->items['value'] + 1
        : 0;
}
function native_complete_property_reference(): int {
    $object = new NativeCompleteObject();
    $reference =& $object->promoted;
    $reference += 2;
    return $object->promoted;
}
function native_complete_static_modes(): int {
    NativeCompleteObject::$staticValue = 38;
    $reference =& NativeCompleteObject::$staticValue;
    $reference++;
    ++NativeCompleteObject::$staticValue;
    NativeCompleteObject::$staticValue += 2;
    return NativeCompleteObject::$staticValue;
}
function native_complete_scope_constant(): int {
    return (new NativeCompleteBase())->scoped() + NativeCompleteBase::ANSWER - 42;
}
function native_complete_trait_alias(): int {
    $object = new NativeCompleteObject();
    return $object->traitValue() + $object->alternateValue() + 1;
}
function native_complete_dynamic_method(): int {
    $object = new NativeCompleteChild();
    $method = 'compute';
    return $object->$method(value: 2);
}
function native_complete_dynamic_static(): int {
    $class = NativeCompleteObject::class;
    $method = 'staticDynamic';
    return $class::$method(40);
}
function native_complete_magic_call(): int {
    $object = new NativeCompleteMagic();
    $method = 'answer';
    return $object->$method(40);
}
function native_complete_magic_static(): int {
    $class = NativeCompleteMagic::class;
    $method = 'answer';
    return $class::$method(40);
}
function native_complete_method_arguments(): int {
    $object = new NativeCompleteObject();
    $value = 39;
    $rest = [2];
    return $object->dynamicRef($value, ...$rest);
}
function native_complete_invoke(): int { return (new NativeCompleteMagic())(40); }
function native_complete_closure_call(): int {
    $object = new NativeCompleteObject();
    $closure = function(int $value): int { return $this->promoted + $value; };
    return $closure->call($object, 2);
}
function native_complete_closure_bind(): int {
    $object = new NativeCompleteObject();
    $closure = function(): int { return $this->promoted + 2; };
    return $closure->bindTo($object, NativeCompleteObject::class)();
}
function native_complete_closure_from_callable(): int {
    return Closure::fromCallable([NativeCompleteObject::class, 'staticDynamic'])(40);
}
function native_complete_callback_array(): int {
    return call_user_func_array([NativeCompleteObject::class, 'staticDynamic'], [40]);
}
function native_complete_callable_check(): int {
    $object = new NativeCompleteMagic();
    return is_callable($object) && is_callable([$object, 'answer']) ? 42 : 0;
}
function native_complete_nested_dynamic(): int {
    $target = 'native_complete_nested_target';
    return $target(40);
}
function native_complete_nested_target(int $value): int {
    $target = 'native_complete_nested_leaf';
    return $target($value);
}
function native_complete_nested_leaf(int $value): int { return $value + 2; }
function native_complete_recursive_closure(): int {
    $sum = function(int $value) use (&$sum): int {
        return $value === 0 ? 0 : 1 + $sum($value - 1);
    };
    return $sum(42);
}
function native_complete_hook(): int {
    $object = new NativeCompleteHooks();
    $object->value = 40;
    return $object->value;
}
function native_complete_readonly(): int { return (new NativeCompleteReadonly(42))->value; }
function native_complete_alias(): int {
    class_alias(NativeCompleteObject::class, 'NativeCompleteAlias');
    $class = 'NativeCompleteAlias';
    return (new $class(40))->compute(2);
}
function native_complete_lazy_ghost(): int {
    $reflection = new ReflectionClass(NativeCompleteObject::class);
    $object = $reflection->newLazyGhost(
        function(NativeCompleteObject $object): void { $object->__construct(40); }
    );
    return $object->compute(2);
}
function native_complete_lazy_proxy(): int {
    $reflection = new ReflectionClass(NativeCompleteObject::class);
    $object = $reflection->newLazyProxy(
        function(NativeCompleteObject $object): NativeCompleteObject {
            return new NativeCompleteObject(40);
        }
    );
    return $object->compute(2);
}
function native_complete_weakmap(): int {
    $object = new NativeCompleteObject(40);
    $map = new WeakMap();
    $map[$object] = 42;
    return $map[$object];
}
function native_complete_internal_handlers(): int {
    $object = new ArrayObject(['value' => 40]);
    $object['value'] += 2;
    return $object['value'];
}
function native_complete_magic_serialization(): int {
    $copy = unserialize(serialize(new NativeCompleteMagic()));
    return (string) $copy === '42' ? 42 : 0;
}
function native_complete_lsb(): int { return NativeCompleteChild::late(); }
function native_complete_dynamic_property(): int {
    $object = new NativeCompleteDynamic();
    $object->value = 42;
    return $object->value;
}
function native_complete_dynamic_property_deprecation(): int {
    $deprecations = 0;
    set_error_handler(
        function(int $severity) use (&$deprecations): bool {
            if ($severity === E_DEPRECATED) {
                $deprecations++;
            }
            return true;
        }
    );
    $object = new NativeCompleteDeprecatedDynamic();
    $object->value = 40;
    restore_error_handler();
    return $object->value + $deprecations + 1;
}
function native_complete_property_modifiers(): int {
    $object = new NativeCompletePropertyModifiers();
    try {
        $object->asymmetric = 1;
    } catch (Error $error) {
        return str_contains($error->getMessage(), 'private(set)')
            && $object->finalValue === 42 ? 42 : 0;
    }
    return 0;
}
function native_complete_object_cast(): int {
    $values = (array) new NativeCompleteObject(40);
    return $values['promoted'] + $values['typed'];
}
function native_complete_debug_info(): int {
    return (new NativeCompleteMagic())->__debugInfo()['value'];
}
function native_complete_missing_constructor(): int {
    return (new NativeCompleteNoConstructor())->value;
}
function native_complete_private_constructor_error(): int {
    try {
        new NativeCompletePrivateConstructor();
    } catch (Error $error) {
        return str_contains($error->getMessage(), 'private') ? 42 : 0;
    }
    return 0;
}
function native_complete_private_constructor(): int {
    return NativeCompletePrivateConstructor::create()->value();
}
function native_complete_forward_static(): int {
    return NativeCompleteChild::forwardCall(40);
}
function native_complete_private_method_error(): int {
    try {
        (new NativeCompletePrivateMethods())->hidden();
    } catch (Error $error) {
        return str_contains($error->getMessage(), 'private') ? 42 : 0;
    }
    return 0;
}
function native_complete_private_static_method_error(): int {
    try {
        NativeCompletePrivateMethods::hiddenStatic();
    } catch (Error $error) {
        return str_contains($error->getMessage(), 'private') ? 42 : 0;
    }
    return 0;
}
function native_complete_invalid_callable_error(): int {
    $callable = [NativeCompletePrivateMethods::class, 'missing'];
    try {
        $callable();
    } catch (Error $error) {
        return 42;
    }
    return 0;
}
function native_complete_throw_finally(): int {
    try {
        throw new RuntimeException('native');
    } catch (RuntimeException $error) {
        $value = $error->getMessage() === 'native' ? 41 : 0;
    } finally {
        $value++;
    }
    return $value;
}

$functions = [
    'native_complete_property_modes',
    'native_complete_property_reference',
    'native_complete_static_modes',
    'native_complete_scope_constant',
    'native_complete_trait_alias',
    'native_complete_dynamic_method',
    'native_complete_dynamic_static',
    'native_complete_magic_call',
    'native_complete_magic_static',
    'native_complete_method_arguments',
    'native_complete_invoke',
    'native_complete_closure_call',
    'native_complete_closure_bind',
    'native_complete_closure_from_callable',
    'native_complete_callback_array',
    'native_complete_callable_check',
    'native_complete_nested_dynamic',
    'native_complete_recursive_closure',
    'native_complete_hook',
    'native_complete_readonly',
    'native_complete_alias',
    'native_complete_lazy_ghost',
    'native_complete_lazy_proxy',
    'native_complete_weakmap',
    'native_complete_internal_handlers',
    'native_complete_magic_serialization',
    'native_complete_lsb',
    'native_complete_dynamic_property',
    'native_complete_dynamic_property_deprecation',
    'native_complete_property_modifiers',
    'native_complete_object_cast',
    'native_complete_debug_info',
    'native_complete_missing_constructor',
    'native_complete_private_constructor_error',
    'native_complete_private_constructor',
    'native_complete_forward_static',
    'native_complete_private_method_error',
    'native_complete_private_static_method_error',
    'native_complete_invalid_callable_error',
    'native_complete_throw_finally',
];
foreach ($functions as $function) {
    printf("%s return=%s\n", $function, json_encode($function()));
}
?>
--EXPECTF--
native_complete_property_modes return=42
native_complete_property_reference return=42
native_complete_static_modes return=42
native_complete_scope_constant return=42
native_complete_trait_alias return=42
native_complete_dynamic_method return=42
native_complete_dynamic_static return=42
native_complete_magic_call return=42
native_complete_magic_static return=42
native_complete_method_arguments return=42
native_complete_invoke return=42
native_complete_closure_call return=42
native_complete_closure_bind return=42
native_complete_closure_from_callable return=42
native_complete_callback_array return=42
native_complete_callable_check return=42
native_complete_nested_dynamic return=42
native_complete_recursive_closure return=42
native_complete_hook return=42
native_complete_readonly return=42
native_complete_alias return=42
native_complete_lazy_ghost return=42
native_complete_lazy_proxy return=42
native_complete_weakmap return=42
native_complete_internal_handlers return=42
native_complete_magic_serialization return=42
native_complete_lsb return=42
native_complete_dynamic_property return=42
native_complete_dynamic_property_deprecation return=42
native_complete_property_modifiers return=42
native_complete_object_cast return=42
native_complete_debug_info return=42
native_complete_missing_constructor return=42
native_complete_private_constructor_error return=42
native_complete_private_constructor return=42
native_complete_forward_static return=42
native_complete_private_method_error return=42
native_complete_private_static_method_error return=42
native_complete_invalid_callable_error return=42
native_complete_throw_finally return=42
