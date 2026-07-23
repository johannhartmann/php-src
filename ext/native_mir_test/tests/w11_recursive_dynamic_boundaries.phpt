--TEST--
Native MIR W11 preserves recursive dynamic compilation and failure recovery
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$directory = sys_get_temp_dir() . '/native-w11-recursive-' . getmypid();
mkdir($directory);
file_put_contents($directory . '/Recovered.php', <<<'PHP'
<?php
class W11RecoveredDynamicClass {
    public static function answer(): int {
        return 42;
    }
}
PHP);
file_put_contents($directory . '/Recursive.php', <<<'PHP'
<?php
class W11RecursiveDynamicClass {
    public static function answer(): int {
        return 42;
    }
}
PHP);

$source = <<<'PHP'
<?php
function w11_recursive_dynamic_boundaries(string $directory): array {
    eval(<<<'CODE'
function w11_dynamic_even(int $value): bool {
    return $value === 0 || w11_dynamic_odd($value - 1);
}
function w11_dynamic_odd(int $value): bool {
    return $value !== 0 && w11_dynamic_even($value - 1);
}
CODE);
    $recursion = [
        w11_dynamic_even(42),
        w11_dynamic_odd(41),
        w11_dynamic_even(41),
    ];

    $failedTrace = [];
    $throwing = static function (string $class) use (&$failedTrace): void {
        $failedTrace[] = 'throw:' . $class;
        throw new LogicException('first loader failed');
    };
    spl_autoload_register($throwing);
    try {
        class_exists('W11RecoveredDynamicClass');
    } catch (LogicException $error) {
        $autoloadFailure = [$error::class, $error->getMessage()];
    } finally {
        spl_autoload_unregister($throwing);
    }

    $recovering = static function (string $class) use ($directory, &$failedTrace): void {
        $failedTrace[] = 'recover:' . $class;
        include $directory . '/Recovered.php';
    };
    spl_autoload_register($recovering);
    try {
        $autoloadRecovery = W11RecoveredDynamicClass::answer();
    } finally {
        spl_autoload_unregister($recovering);
    }

    $recursiveTrace = [];
    $recursive = static function (string $class) use ($directory, &$recursiveTrace): void {
        $recursiveTrace[] = $class;
        $recursiveTrace[] = class_exists($class);
        include $directory . '/Recursive.php';
    };
    spl_autoload_register($recursive);
    try {
        $recursiveAutoload = W11RecursiveDynamicClass::answer();
    } finally {
        spl_autoload_unregister($recursive);
    }

    eval('function w11_reserved_dynamic_generator(): Generator { yield 42; }');
    try {
        iterator_to_array(w11_reserved_dynamic_generator());
        $generatorFailure = null;
    } catch (Error $error) {
        $generatorFailure = str_contains(
            $error->getMessage(),
            'reserved for native W12 activation',
        );
    }
    $afterFailure = eval('return 42;');

    return [
        $recursion,
        $autoloadFailure,
        $autoloadRecovery,
        $failedTrace,
        $recursiveAutoload,
        $recursiveTrace,
        $generatorFailure,
        $afterFailure,
    ];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11-recursive-dynamic-boundaries.php',
    [$directory],
    ['wave' => 11, 'function' => 'w11_recursive_dynamic_boundaries'],
);
if ($result['status'] !== 'accepted') {
    printf("diagnostics=%s\n", json_encode($result['diagnostics'] ?? null));
}
printf(
    "%s return=%s vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value'] ?? null),
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
);

unlink($directory . '/Recovered.php');
unlink($directory . '/Recursive.php');
rmdir($directory);
?>
--EXPECT--
accepted return=[[true,true,false],["LogicException","first loader failed"],42,["throw:W11RecoveredDynamicClass","recover:W11RecoveredDynamicClass"],42,["W11RecursiveDynamicClass",false],true,42] vm=0 execute_ex=0 handler=0
