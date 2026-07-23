--TEST--
Native MIR W11 preserves include, require, once, path, and nested codeunit semantics
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$directory = sys_get_temp_dir() . '/native-w11-includes-' . getmypid();
$library = $directory . '/library';
mkdir($directory);
mkdir($library);

file_put_contents($directory . '/once.php', <<<'PHP'
<?php
function w11_once_value(): int {
    return 40;
}
return 41;
PHP);
file_put_contents($library . '/path-unit.php', <<<'PHP'
<?php
return 42;
PHP);
file_put_contents($directory . '/nested-inner.php', <<<'PHP'
<?php
return 40;
PHP);
file_put_contents($directory . '/nested-outer.php', <<<'PHP'
<?php
return include __DIR__ . '/nested-inner.php';
PHP);
file_put_contents($directory . '/mutable.php', "<?php return 1;\n");

$source = <<<'PHP'
<?php
function w11_once_identity(string $file): array {
    $first = require $file;
    $second = require_once dirname($file) . '/./once.php';
    return [$first, $second, w11_once_value() + 2];
}
function w11_include_path(string $directory): int {
    $previous = set_include_path($directory);
    try {
        return include 'path-unit.php';
    } finally {
        set_include_path($previous);
    }
}
function w11_nested_include(string $file): int {
    return require $file;
}
function w11_recompile_changed_file(string $file): array {
    $first = include $file;
    file_put_contents($file, "<?php return 2;\n");
    clearstatcache(true, $file);
    $second = include $file;
    return [$first, $second];
}
function w11_missing_include(string $file): array {
    set_error_handler(static fn (int $severity, string $message): bool => true);
    try {
        $included = include $file;
        try {
            require $file;
            $required = 'unreachable';
        } catch (Throwable $error) {
            $required = get_class($error);
        }
        return [$included, $required];
    } finally {
        restore_error_handler();
    }
}
PHP;

$cases = [
    ['w11_once_identity', [$directory . '/once.php']],
    ['w11_include_path', [$library]],
    ['w11_nested_include', [$directory . '/nested-outer.php']],
    ['w11_recompile_changed_file', [$directory . '/mutable.php']],
    ['w11_missing_include', [$directory . '/missing.php']],
];

foreach ($cases as [$function, $arguments]) {
    $result = native_mir_test_compile_execute(
        $source,
        "w11-include-$function.php",
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

foreach ([
    $directory . '/once.php',
    $library . '/path-unit.php',
    $directory . '/nested-inner.php',
    $directory . '/nested-outer.php',
    $directory . '/mutable.php',
] as $file) {
    unlink($file);
}
rmdir($library);
rmdir($directory);
?>
--EXPECT--
w11_once_identity accepted return=[41,true,42] vm=0 execute_ex=0 handler=0
w11_include_path accepted return=42 vm=0 execute_ex=0 handler=0
w11_nested_include accepted return=40 vm=0 execute_ex=0 handler=0
w11_recompile_changed_file accepted return=[1,2] vm=0 execute_ex=0 handler=0
w11_missing_include accepted return=[false,"Error"] vm=0 execute_ex=0 handler=0
