--TEST--
Native MIR W10 executes complete object and class fundamentals without VM dispatch
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
interface W10Contract { public function value(): int; }
trait W10Trait { public function value(): int { return $this->stored; } }
class W10Base {
    public static int $base = 40;
    public static function late(): int { return static::$base + 2; }
}
final class W10Object extends W10Base implements W10Contract {
    use W10Trait;
    public function __construct(public int $stored) {}
}
final readonly class W10Readonly {
    public function __construct(public int $value) {}
}
final class W10MagicProperties {
    private array $values = [];
    public function __set(string $name, mixed $value): void { $this->values[$name] = $value; }
    public function __get(string $name): mixed { return $this->values[$name]; }
    public function __isset(string $name): bool { return isset($this->values[$name]); }
    public function __unset(string $name): void { unset($this->values[$name]); }
}
final class W10Cloneable {
    public function __construct(public int $value) {}
    public function __clone() { $this->value++; }
}
final class W10Destructible {
    public static int $value = 40;
    public function __destruct() { self::$value += 2; }
}
enum W10Number: int {
    case Answer = 42;
    public function number(): int { return $this->value; }
}
function w10_typed_property(): int { return (new W10Object(42))->stored; }
function w10_readonly_property(): int { return (new W10Readonly(42))->value; }
function w10_magic_property(): int { $o = new W10MagicProperties(); $o->answer = 42; return isset($o->answer) ? $o->answer : 0; }
function w10_magic_unset(): int { $o = new W10MagicProperties(); $o->answer = 42; unset($o->answer); return empty($o->answer) ? 42 : 0; }
function w10_clone(): int { $copy = clone new W10Cloneable(41); return $copy->value; }
function w10_lsb(): int { return W10Object::late(); }
function w10_trait_interface(): int { $o = new W10Object(42); return $o instanceof W10Contract ? $o->value() : 0; }
function w10_enum(): int { return W10Number::Answer->number(); }
function w10_anon(): int { $o = new class(42) { public function __construct(public int $v) {} }; return $o->v; }
function w10_nullsafe_object(): int { $o = new W10Object(42); return $o?->value(); }
function w10_nullsafe_null(): int { $o = null; return $o?->value() ?? 42; }
function w10_compare(): int { return new W10Object(42) == new W10Object(42) ? 42 : 0; }
function w10_destructor(): int { $o = new W10Destructible(); unset($o); return W10Destructible::$value; }
function w10_weakref(): int { $o = new W10Object(42); return WeakReference::create($o)->get()->stored; }
PHP;

$functions = [
    'w10_typed_property', 'w10_readonly_property', 'w10_magic_property',
    'w10_magic_unset', 'w10_clone', 'w10_lsb', 'w10_trait_interface',
    'w10_enum', 'w10_anon', 'w10_nullsafe_object', 'w10_nullsafe_null',
    'w10_compare', 'w10_destructor', 'w10_weakref',
];
foreach ($functions as $function) {
    $result = native_mir_test_compile_execute(
        $source,
        'w10-objects.php',
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
w10_typed_property accepted return=42 vm=0 execute_ex=0 handler=0
w10_readonly_property accepted return=42 vm=0 execute_ex=0 handler=0
w10_magic_property accepted return=42 vm=0 execute_ex=0 handler=0
w10_magic_unset accepted return=42 vm=0 execute_ex=0 handler=0
w10_clone accepted return=42 vm=0 execute_ex=0 handler=0
w10_lsb accepted return=42 vm=0 execute_ex=0 handler=0
w10_trait_interface accepted return=42 vm=0 execute_ex=0 handler=0
w10_enum accepted return=42 vm=0 execute_ex=0 handler=0
w10_anon accepted return=42 vm=0 execute_ex=0 handler=0
w10_nullsafe_object accepted return=42 vm=0 execute_ex=0 handler=0
w10_nullsafe_null accepted return=42 vm=0 execute_ex=0 handler=0
w10_compare accepted return=42 vm=0 execute_ex=0 handler=0
w10_destructor accepted return=42 vm=0 execute_ex=0 handler=0
w10_weakref accepted return=42 vm=0 execute_ex=0 handler=0
