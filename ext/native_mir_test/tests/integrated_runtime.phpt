--TEST--
Native MIR executes the integrated object and callable runtime without VM dispatch
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
class NativeIntegratedLoadedBase {
    protected int $base = 30;
    public function base(): int { return $this->base; }
}
function native_integrated_loaded_callback(int $value): int { return $value + 1; }

$source = <<<'PHP'
<?php
interface NativeIntegratedContract { public function compute(int $value): int; }
trait NativeIntegratedTrait {
    public function compute(int $value): int { return $this->base() + $value; }
}
class NativeIntegratedObject extends NativeIntegratedLoadedBase implements NativeIntegratedContract {
    use NativeIntegratedTrait;
    private array $dynamic = [];
    public static int $destructed = 0;
    public function __construct(public readonly int $offset) {}
    public function __get(string $name): mixed { return $this->dynamic[$name] ?? null; }
    public function __set(string $name, mixed $value): void { $this->dynamic[$name] = $value; }
    public function __destruct() {
        self::$destructed = native_integrated_loaded_callback(self::$destructed);
    }
}
function native_integrated_runtime(): int {
    NativeIntegratedObject::$destructed = 39;
    $class = 'NativeIntegratedObject';
    $object = new $class(10);
    $object->answer = 40;
    $captured = 1;
    $callback = function(int $value) use (&$captured, $object): int {
        $captured++;
        $method = 'compute';
        return $object->$method($value) + $captured;
    };
    $mapped = array_map($callback, [8]);
    $null = null;
    $anonymous = new class { public function value(): int { return 1; } };
    try {
        if (($null?->missing()?->value ?? 1) !== 1) {
            return 0;
        }
        throw new RuntimeException('integrated');
    } catch (RuntimeException $error) {
        $value = $error->getMessage() === 'integrated'
            ? $mapped[0] + $anonymous->value() + $object->answer - 40
            : 0;
    } finally {
        $value += 1;
    }
    unset($object, $callback);
    gc_collect_cycles();
    return $value + NativeIntegratedObject::$destructed - 40;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'integrated-runtime.php',
    [],
    ['function' => 'native_integrated_runtime'],
);
printf(
    "status=%s return=%s vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value'] ?? null),
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
);
?>
--EXPECT--
status=accepted return=42 vm=0 execute_ex=0 handler=0
