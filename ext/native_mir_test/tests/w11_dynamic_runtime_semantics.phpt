--TEST--
Native MIR W11 executes dynamic declarations, bindings, autoload, and eval errors
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$directory = sys_get_temp_dir() . '/native-w11-runtime-' . getmypid();
mkdir($directory);

file_put_contents($directory . '/base.php', <<<'PHP'
<?php
class W11RuntimeBase {
    public function base(): int {
        return 40;
    }
}
PHP);
file_put_contents($directory . '/child.php', <<<'PHP'
<?php
class W11RuntimeChild extends W11RuntimeBase {
    public function value(): int {
        return $this->base() + 2;
    }
}
PHP);

$cases = [
    [
        <<<'PHP'
<?php
function w11_runtime_declarations(): array {
    eval(<<<'CODE'
interface W11RuntimeContract {
    public function value(): int;
}
trait W11RuntimeTrait {
    public function value(): int {
        return W11_RUNTIME_CONSTANT;
    }
}
enum W11RuntimeEnum: int {
    case Answer = 42;
}
class W11RuntimeImplementation implements W11RuntimeContract {
    use W11RuntimeTrait;
}
const W11_RUNTIME_CONSTANT = 42;
function w11_runtime_function(): int {
    static $calls = 40;
    return ++$calls;
}
CODE);
    $object = new W11RuntimeImplementation();
    return [
        $object->value(),
        W11RuntimeEnum::Answer->value,
        w11_runtime_function(),
        w11_runtime_function(),
    ];
}
PHP,
        'w11_runtime_declarations',
        [],
    ],
    [
        <<<'PHP'
<?php
function w11_runtime_eval_bindings(): array {
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
        'w11_runtime_eval_bindings',
        [],
    ],
    [
        <<<'PHP'
<?php
function w11_runtime_autoload(string $directory): array {
    $trace = [];
    $first = static function (string $class) use (&$trace): void {
        $trace[] = 'first:' . $class;
    };
    $second = static function (string $class) use ($directory, &$trace): void {
        $trace[] = 'second:' . $class;
        if ($class === 'W11RuntimeChild') {
            include_once $directory . '/child.php';
        } elseif ($class === 'W11RuntimeBase') {
            include_once $directory . '/base.php';
        }
    };
    spl_autoload_register($first);
    spl_autoload_register($second);
    try {
        $value = (new W11RuntimeChild())->value();
        return [$value, $trace];
    } finally {
        spl_autoload_unregister($second);
        spl_autoload_unregister($first);
    }
}
PHP,
        'w11_runtime_autoload',
        [$directory],
    ],
    [
        <<<'PHP'
<?php
function w11_runtime_eval_errors(): array {
    $parse = null;
    try {
        eval('this is not valid PHP');
    } catch (ParseError $error) {
        $parse = $error::class;
    }
    return [$parse, eval('return 42;')];
}
PHP,
        'w11_runtime_eval_errors',
        [],
    ],
];

foreach ($cases as $index => [$source, $function, $arguments]) {
    $result = native_mir_test_compile_execute(
        $source,
        "w11-runtime-$index.php",
        $arguments,
        ['wave' => 11, 'function' => $function],
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
w11_runtime_declarations accepted return=[42,42,41,42] vm=0 execute_ex=0 handler=0
w11_runtime_eval_bindings accepted return=[[42,42],42] vm=0 execute_ex=0 handler=0
w11_runtime_autoload accepted return=[42,["first:W11RuntimeChild","second:W11RuntimeChild","first:W11RuntimeBase","second:W11RuntimeBase"]] vm=0 execute_ex=0 handler=0
w11_runtime_eval_errors accepted return=["ParseError",42] vm=0 execute_ex=0 handler=0
