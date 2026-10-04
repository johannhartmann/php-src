--TEST--
Native MIR preserves recursive dynamic compilation and failure recovery
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$directory = sys_get_temp_dir() . '/native-recursive-' . getmypid();
mkdir($directory);
file_put_contents($directory . '/Recovered.php', <<<'PHP'
<?php
class NativeRecoveredDynamicClass {
    public static function answer(): int {
        return 42;
    }
}
PHP);
file_put_contents($directory . '/Recursive.php', <<<'PHP'
<?php
class NativeRecursiveDynamicClass {
    public static function answer(): int {
        return 42;
    }
}
PHP);

$source = <<<'PHP'
<?php
function native_recursive_dynamic_boundaries(string $directory): array {
    eval(<<<'CODE'
function native_dynamic_even(int $value): bool {
    return $value === 0 || native_dynamic_odd($value - 1);
}
function native_dynamic_odd(int $value): bool {
    return $value !== 0 && native_dynamic_even($value - 1);
}
CODE);
    $recursion = [
        native_dynamic_even(42),
        native_dynamic_odd(41),
        native_dynamic_even(41),
    ];

    $failedTrace = [];
    $throwing = static function (string $class) use (&$failedTrace): void {
        $failedTrace[] = 'throw:' . $class;
        throw new LogicException('first loader failed');
    };
    spl_autoload_register($throwing);
    try {
        class_exists('NativeRecoveredDynamicClass');
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
        $autoloadRecovery = NativeRecoveredDynamicClass::answer();
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
        $recursiveAutoload = NativeRecursiveDynamicClass::answer();
    } finally {
        spl_autoload_unregister($recursive);
    }

    eval('function native_reserved_dynamic_generator(): Generator { yield 42; }');
    $generatorActivation = iterator_to_array(
        native_reserved_dynamic_generator(),
    ) === [42];
    $afterFailure = eval('return 42;');

    return [
        $recursion,
        $autoloadFailure,
        $autoloadRecovery,
        $failedTrace,
        $recursiveAutoload,
        $recursiveTrace,
        $generatorActivation,
        $afterFailure,
    ];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'recursive-dynamic-boundaries.php',
    [$directory],
    ['function' => 'native_recursive_dynamic_boundaries'],
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
accepted return=[[true,true,false],["LogicException","first loader failed"],42,["throw:NativeRecoveredDynamicClass","recover:NativeRecoveredDynamicClass"],42,["NativeRecursiveDynamicClass",false],true,42] vm=0 execute_ex=0 handler=0
