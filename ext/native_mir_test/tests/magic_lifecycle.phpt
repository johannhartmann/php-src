--TEST--
Native MIR executes magic, lifecycle, error, and weak-GC semantics
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
class NativeMagicAll {
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
class NativeLegacyMagic {
    public int $value = 40;
    public function __sleep(): array { return ['value']; }
    public function __wakeup(): void { $this->value += 2; }
}
class NativeTyped { public int $value; }
class NativeReadonly { public function __construct(public readonly int $value = 40) {} }
class NativePrivate { private int $value = 42; }
class NativeThrowDestruct { public function __destruct() { throw new RuntimeException('dtor'); } }
class NativeCycle { public ?self $next = null; }
function native_magic_properties(): int { $o = new NativeMagicAll(); $o->answer = 42; $ok = isset($o->answer) && $o->answer === 42; unset($o->answer); return $ok && !isset($o->answer) ? 42 : 0; }
function native_magic_clone(): int { return (clone new NativeMagicAll())->value; }
function native_magic_serialize_new(): int { return unserialize(serialize(new NativeMagicAll()))->value; }
function native_magic_serialize_legacy(): int { return unserialize(serialize(new NativeLegacyMagic()))->value; }
function native_magic_set_state(): int { return NativeMagicAll::__set_state(['value' => 40])->value; }
function native_magic_string(): int { return (int) (string) new NativeMagicAll(40); }
function native_destructor(): int { NativeMagicAll::$destructed = 40; $o = new NativeMagicAll(); unset($o); return NativeMagicAll::$destructed; }
function native_destructor_exception(): int { try { $o = new NativeThrowDestruct(); unset($o); } catch (RuntimeException $e) { return $e->getMessage() === 'dtor' ? 42 : 0; } return 0; }
function native_typed_error(): int { try { return (new NativeTyped())->value; } catch (Error $e) { return str_contains($e->getMessage(), 'must not be accessed') ? 42 : 0; } }
function native_readonly_error(): int { $o = new NativeReadonly(); try { $o->value = 1; } catch (Error $e) { return str_contains($e->getMessage(), 'readonly') ? 42 : 0; } return 0; }
function native_visibility_error(): int { try { return (new NativePrivate())->value; } catch (Error $e) { return str_contains($e->getMessage(), 'private') ? 42 : 0; } }
function native_weak_cycle(): int { $a = new NativeCycle(); $b = new NativeCycle(); $a->next = $b; $b->next = $a; $weak = WeakReference::create($a); unset($a, $b); gc_collect_cycles(); return $weak->get() === null ? 42 : 0; }
PHP;

foreach ([
    'native_magic_properties', 'native_magic_clone', 'native_magic_serialize_new',
    'native_magic_serialize_legacy', 'native_magic_set_state', 'native_magic_string',
    'native_destructor', 'native_destructor_exception', 'native_typed_error',
    'native_readonly_error', 'native_visibility_error', 'native_weak_cycle',
] as $function) {
    $result = native_mir_test_compile_execute(
        $source,
        'magic-lifecycle.php',
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
native_magic_properties accepted return=42 vm=0 execute_ex=0 handler=0
native_magic_clone accepted return=42 vm=0 execute_ex=0 handler=0
native_magic_serialize_new accepted return=42 vm=0 execute_ex=0 handler=0
native_magic_serialize_legacy accepted return=42 vm=0 execute_ex=0 handler=0
native_magic_set_state accepted return=42 vm=0 execute_ex=0 handler=0
native_magic_string accepted return=42 vm=0 execute_ex=0 handler=0
native_destructor accepted return=42 vm=0 execute_ex=0 handler=0
native_destructor_exception accepted return=42 vm=0 execute_ex=0 handler=0
native_typed_error accepted return=42 vm=0 execute_ex=0 handler=0
native_readonly_error accepted return=42 vm=0 execute_ex=0 handler=0
native_visibility_error accepted return=42 vm=0 execute_ex=0 handler=0
native_weak_cycle accepted return=42 vm=0 execute_ex=0 handler=0
