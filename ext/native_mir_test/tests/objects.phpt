--TEST--
Native code executes complete object and class fundamentals without VM dispatch
--EXTENSIONS--
native_mir_test
--FILE--
<?php
interface NativeContract { public function value(): int; }
trait NativeTrait { public function value(): int { return $this->stored; } }
class NativeBase {
    public static int $base = 40;
    public static function late(): int { return static::$base + 2; }
}
final class NativeObject extends NativeBase implements NativeContract {
    use NativeTrait;
    public function __construct(public int $stored) {}
}
final readonly class NativeReadonly {
    public function __construct(public int $value) {}
}
final class NativeMagicProperties {
    private array $values = [];
    public function __set(string $name, mixed $value): void { $this->values[$name] = $value; }
    public function __get(string $name): mixed { return $this->values[$name]; }
    public function __isset(string $name): bool { return isset($this->values[$name]); }
    public function __unset(string $name): void { unset($this->values[$name]); }
}
final class NativeCloneable {
    public function __construct(public int $value) {}
    public function __clone() { $this->value++; }
}
final class NativeDestructible {
    public static int $value = 40;
    public function __destruct() { self::$value += 2; }
}
enum NativeNumber: int {
    case Answer = 42;
    public function number(): int { return $this->value; }
}
function native_typed_property(): int { return (new NativeObject(42))->stored; }
function native_readonly_property(): int { return (new NativeReadonly(42))->value; }
function native_magic_property(): int { $o = new NativeMagicProperties(); $o->answer = 42; return isset($o->answer) ? $o->answer : 0; }
function native_magic_unset(): int { $o = new NativeMagicProperties(); $o->answer = 42; unset($o->answer); return empty($o->answer) ? 42 : 0; }
function native_clone(): int { $copy = clone new NativeCloneable(41); return $copy->value; }
function native_lsb(): int { return NativeObject::late(); }
function native_trait_interface(): int { $o = new NativeObject(42); return $o instanceof NativeContract ? $o->value() : 0; }
function native_enum(): int { return NativeNumber::Answer->number(); }
function native_anon(): int { $o = new class(42) { public function __construct(public int $v) {} }; return $o->v; }
function native_nullsafe_object(): int { $o = new NativeObject(42); return $o?->value(); }
function native_nullsafe_null(): int { $o = null; return $o?->value() ?? 42; }
function native_compare(): int { return new NativeObject(42) == new NativeObject(42) ? 42 : 0; }
function native_destructor(): int { $o = new NativeDestructible(); unset($o); return NativeDestructible::$value; }
function native_weakref(): int { $o = new NativeObject(42); return WeakReference::create($o)->get()->stored; }

$functions = [
    'native_typed_property', 'native_readonly_property', 'native_magic_property',
    'native_magic_unset', 'native_clone', 'native_lsb', 'native_trait_interface',
    'native_enum', 'native_anon', 'native_nullsafe_object', 'native_nullsafe_null',
    'native_compare', 'native_destructor', 'native_weakref',
];
foreach ($functions as $function) {
    printf("%s return=%s\n", $function, json_encode($function()));
}
?>
--EXPECTF--
native_typed_property return=42
native_readonly_property return=42
native_magic_property return=42
native_magic_unset return=42
native_clone return=42
native_lsb return=42
native_trait_interface return=42
native_enum return=42
native_anon return=42
native_nullsafe_object return=42
native_nullsafe_null return=42
native_compare return=42
native_destructor return=42
native_weakref return=42
