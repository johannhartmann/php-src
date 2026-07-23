--TEST--
Native MIR W10 executes magic, lifecycle, error, and weak-GC semantics
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
class W10MagicAll {
    public array $data = [];
    public static int $destructed = 0;
    public function __construct(public int $value = 40) {}
    public function __destruct() { self::$destructed += 2; }
    public function __get(string $name): mixed { return $this->data[$name] ?? 0; }
    public function __set(string $name, mixed $value): void { $this->data[$name] = $value; }
    public function __isset(string $name): bool { return isset($this->data[$name]); }
    public function __unset(string $name): void { unset($this->data[$name]); }
    public function __clone() { $this->value += 2; }
    public function __serialize(): array { return ['value' => $this->value]; }
    public function __unserialize(array $data): void { $this->value = $data['value'] + 2; }
    public static function __set_state(array $data): object { return new self($data['value'] + 2); }
    public function __debugInfo(): array { return ['value' => $this->value + 2]; }
    public function __toString(): string { return (string) ($this->value + 2); }
}
class W10LegacyMagic {
    public int $value = 40;
    public function __sleep(): array { return ['value']; }
    public function __wakeup(): void { $this->value += 2; }
}
class W10Typed { public int $value; }
class W10Readonly { public function __construct(public readonly int $value = 40) {} }
class W10Private { private int $value = 42; }
class W10ThrowDestruct { public function __destruct() { throw new RuntimeException('dtor'); } }
class W10Cycle { public ?self $next = null; }
function w10_magic_properties(): int { $o = new W10MagicAll(); $o->answer = 42; $ok = isset($o->answer) && $o->answer === 42; unset($o->answer); return $ok && !isset($o->answer) ? 42 : 0; }
function w10_magic_clone(): int { return (clone new W10MagicAll())->value; }
function w10_magic_serialize_new(): int { return unserialize(serialize(new W10MagicAll()))->value; }
function w10_magic_serialize_legacy(): int { return unserialize(serialize(new W10LegacyMagic()))->value; }
function w10_magic_set_state(): int { return W10MagicAll::__set_state(['value' => 40])->value; }
function w10_magic_string(): int { return (int) (string) new W10MagicAll(40); }
function w10_destructor(): int { W10MagicAll::$destructed = 40; $o = new W10MagicAll(); unset($o); return W10MagicAll::$destructed; }
function w10_destructor_exception(): int { try { $o = new W10ThrowDestruct(); unset($o); } catch (RuntimeException $e) { return $e->getMessage() === 'dtor' ? 42 : 0; } return 0; }
function w10_typed_error(): int { try { return (new W10Typed())->value; } catch (Error $e) { return str_contains($e->getMessage(), 'must not be accessed') ? 42 : 0; } }
function w10_readonly_error(): int { $o = new W10Readonly(); try { $o->value = 1; } catch (Error $e) { return str_contains($e->getMessage(), 'readonly') ? 42 : 0; } return 0; }
function w10_visibility_error(): int { try { return (new W10Private())->value; } catch (Error $e) { return str_contains($e->getMessage(), 'private') ? 42 : 0; } }
function w10_weak_cycle(): int { $a = new W10Cycle(); $b = new W10Cycle(); $a->next = $b; $b->next = $a; $weak = WeakReference::create($a); unset($a, $b); gc_collect_cycles(); return $weak->get() === null ? 42 : 0; }
PHP;

foreach ([
    'w10_magic_properties', 'w10_magic_clone', 'w10_magic_serialize_new',
    'w10_magic_serialize_legacy', 'w10_magic_set_state', 'w10_magic_string',
    'w10_destructor', 'w10_destructor_exception', 'w10_typed_error',
    'w10_readonly_error', 'w10_visibility_error', 'w10_weak_cycle',
] as $function) {
    $result = native_mir_test_compile_execute(
        $source,
        'w10-magic-lifecycle.php',
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
w10_magic_properties accepted return=42 vm=0 execute_ex=0 handler=0
w10_magic_clone accepted return=42 vm=0 execute_ex=0 handler=0
w10_magic_serialize_new accepted return=42 vm=0 execute_ex=0 handler=0
w10_magic_serialize_legacy accepted return=42 vm=0 execute_ex=0 handler=0
w10_magic_set_state accepted return=42 vm=0 execute_ex=0 handler=0
w10_magic_string accepted return=42 vm=0 execute_ex=0 handler=0
w10_destructor accepted return=42 vm=0 execute_ex=0 handler=0
w10_destructor_exception accepted return=42 vm=0 execute_ex=0 handler=0
w10_typed_error accepted return=42 vm=0 execute_ex=0 handler=0
w10_readonly_error accepted return=42 vm=0 execute_ex=0 handler=0
w10_visibility_error accepted return=42 vm=0 execute_ex=0 handler=0
w10_weak_cycle accepted return=42 vm=0 execute_ex=0 handler=0
