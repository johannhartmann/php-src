--TEST--
Native MIR executes dynamic declarations, bindings, autoload, and eval errors
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$directory = sys_get_temp_dir() . '/native-runtime-' . getmypid();
mkdir($directory);

file_put_contents($directory . '/base.php', <<<'PHP'
<?php
class NativeRuntimeBase {
    public function base(): int {
        return 40;
    }
}
PHP);
file_put_contents($directory . '/child.php', <<<'PHP'
<?php
class NativeRuntimeChild extends NativeRuntimeBase {
    public function value(): int {
        return $this->base() + 2;
    }
}
PHP);

$cases = [
    [
        <<<'PHP'
<?php
function native_runtime_declarations(): array {
    eval(<<<'CODE'
interface NativeRuntimeContract {
    public function value(): int;
}
trait NativeRuntimeTrait {
    public function value(): int {
        return NATIVE_RUNTIME_CONSTANT;
    }
}
enum NativeRuntimeEnum: int {
    case Answer = 42;
}
class NativeRuntimeImplementation implements NativeRuntimeContract {
    use NativeRuntimeTrait;
}
const NATIVE_RUNTIME_CONSTANT = 42;
function native_runtime_function(): int {
    static $calls = 40;
    return ++$calls;
}
CODE);
    $object = new NativeRuntimeImplementation();
    return [
        $object->value(),
        NativeRuntimeEnum::Answer->value,
        native_runtime_function(),
        native_runtime_function(),
    ];
}
PHP,
        'native_runtime_declarations',
        [],
    ],
    [
        <<<'PHP'
<?php
function native_runtime_eval_bindings(): array {
    $captured = 39;
    $name = 'captured';
    $closure = function () use (&$captured, $name): array {
        $result = eval(
            '$$name += 3; '
            . '$values = compact("captured"); '
            . '$incoming = ["created" => 42]; '
            . 'extract($incoming); '
            . 'return [$values["captured"], $created];'
        );
        return [$result, $captured];
    };
    return $closure();
}
PHP,
        'native_runtime_eval_bindings',
        [],
    ],
    [
        <<<'PHP'
<?php
function native_runtime_autoload(string $directory): array {
    $trace = [];
    $first = static function (string $class) use (&$trace): void {
        $trace[] = 'first:' . $class;
    };
    $second = static function (string $class) use ($directory, &$trace): void {
        $trace[] = 'second:' . $class;
        if ($class === 'NativeRuntimeChild') {
            include_once $directory . '/child.php';
        } elseif ($class === 'NativeRuntimeBase') {
            include_once $directory . '/base.php';
        }
    };
    spl_autoload_register($first);
    spl_autoload_register($second);
    try {
        $value = (new NativeRuntimeChild())->value();
        return [$value, $trace];
    } finally {
        spl_autoload_unregister($second);
        spl_autoload_unregister($first);
    }
}
PHP,
        'native_runtime_autoload',
        [$directory],
    ],
    [
        <<<'PHP'
<?php
function native_runtime_eval_errors(): array {
    $parse = null;
    try {
        eval('this is not valid PHP');
    } catch (ParseError $error) {
        $parse = $error::class;
    }
    return [$parse, eval('return 42;')];
}
PHP,
        'native_runtime_eval_errors',
        [],
    ],
];

foreach ($cases as $index => [$source, $function, $arguments]) {
    $result = native_mir_test_compile_execute(
        $source,
        "runtime-$index.php",
        $arguments,
        ['function' => $function],
    );
    if ($result['status'] !== 'accepted') {
        printf(
            "%s diagnostics=%s\n",
            $function,
            json_encode($result['diagnostics'] ?? null),
        );
    }
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

unlink($directory . '/base.php');
unlink($directory . '/child.php');
rmdir($directory);
?>
--EXPECT--
native_runtime_declarations accepted return=[42,42,41,42] vm=0 execute_ex=0 handler=0
native_runtime_eval_bindings accepted return=[[42,42],42] vm=0 execute_ex=0 handler=0
native_runtime_autoload accepted return=[42,["first:NativeRuntimeChild","second:NativeRuntimeChild","first:NativeRuntimeBase","second:NativeRuntimeBase"]] vm=0 execute_ex=0 handler=0
native_runtime_eval_errors accepted return=["ParseError",42] vm=0 execute_ex=0 handler=0
