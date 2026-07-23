--TEST--
Native MIR W11 executes a complete multi-file dynamic package without VM fallback
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$directory = sys_get_temp_dir() . '/native-w11-package-' . getmypid();
mkdir($directory);

file_put_contents($directory . '/Base.php', <<<'PHP'
<?php
namespace W11Package;

interface Contract {
    public function run(array &$trace): array;
}

trait Computes {
    protected function base(): int {
        return 39;
    }
}

class Base {
    use Computes;
}
PHP);

file_put_contents($directory . '/Loaded.php', <<<'PHP'
<?php
namespace W11Package;

class Loaded extends Base implements Contract {
    public function run(array &$trace): array {
        $captured = 0;
        $local = 39;
        $name = 'local';
        $reference =& $local;
        $callback = static function (int $value) use (&$captured, &$trace): int {
            $trace[] = 'callback:' . $value;
            return $value + ++$captured;
        };
        try {
            $value = eval(
                '$$name++; '
                . '$alias =& $reference; '
                . 'return $callback($alias);'
            );
            $mapped = array_map($callback, [40]);
            throw new \RuntimeException('dynamic');
        } catch (\RuntimeException $error) {
            $trace[] = 'catch:' . $error->getMessage();
        } finally {
            $trace[] = 'finally';
        }
        return [$value, $mapped[0], $local, $captured];
    }
}
PHP);

$source = <<<'PHP'
<?php
function w11_package_without_cvs(): int {
    return eval('return 42;');
}

function w11_complete_package(string $directory): array {
    $trace = [];
    $first = static function (string $class) use (&$trace): void {
        $trace[] = 'first:' . $class;
    };
    $second = static function (string $class) use ($directory, &$trace): void {
        $trace[] = 'second:' . $class;
        if ($class === 'W11Package\Loaded') {
            include_once $directory . '/Loaded.php';
        } elseif ($class === 'W11Package\Base') {
            include_once $directory . '/Base.php';
        }
    };
    spl_autoload_register($first);
    spl_autoload_register($second);
    try {
        $loaded = new W11Package\Loaded();
        $firstRun = $loaded->run($trace);
        $secondRun = $loaded->run($trace);
    } finally {
        spl_autoload_unregister($second);
        spl_autoload_unregister($first);
    }

    $runtime = eval(<<<'CODE'
namespace W11Package;
enum DynamicAnswer: int {
    case Answer = 42;
}
const DYNAMIC_VALUE = 42;
function dynamic_static(): int {
    static $calls = 40;
    return ++$calls;
}
return [
    DynamicAnswer::Answer->value,
    DYNAMIC_VALUE,
    dynamic_static(),
    dynamic_static(),
];
CODE);
    $evalOne = eval('static $value = 0; return ++$value;');
    $evalTwo = eval('static $value = 0; return ++$value;');
    $nested = eval('return eval(\'return 42;\');');

    $throwing = static function (string $class): void {
        if ($class === 'W11Package\NeverLoaded') {
            throw new LogicException('autoload');
        }
    };
    spl_autoload_register($throwing, true, true);
    try {
        class_exists('W11Package\NeverLoaded');
        $autoloadError = null;
    } catch (LogicException $error) {
        $autoloadError = [$error::class, $error->getMessage()];
    } finally {
        spl_autoload_unregister($throwing);
    }

    return [
        $firstRun,
        $secondRun,
        $runtime,
        [$evalOne, $evalTwo],
        $nested,
        w11_package_without_cvs(),
        $autoloadError,
        $trace,
    ];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11-complete-package.php',
    [$directory],
    ['wave' => 11, 'function' => 'w11_complete_package'],
);
if ($result['status'] !== 'accepted') {
    printf("diagnostics=%s\n", json_encode($result['diagnostics'] ?? null));
}
printf(
    "%s return=%s units=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value'] ?? null),
    $result['execution']['native_codeunits'] ?? -1,
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
);

unlink($directory . '/Loaded.php');
unlink($directory . '/Base.php');
rmdir($directory);
?>
--EXPECTF--
accepted return=[[41,42,40,2],[41,42,40,2],[42,42,41,42],[1,1],42,42,["LogicException","autoload"],["first:W11Package\\Loaded","second:W11Package\\Loaded","first:W11Package\\Base","second:W11Package\\Base","callback:40","callback:40","catch:dynamic","finally","callback:40","callback:40","catch:dynamic","finally"]] units=%d vm=0 execute_ex=0 handler=0
