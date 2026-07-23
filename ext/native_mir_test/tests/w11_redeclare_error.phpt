--TEST--
Native MIR W11 preserves fatal redeclaration semantics without VM fallback
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
function w11_redeclare_error(): void {
    eval('function w11_redeclared(): int { return 42; }');
    eval('function w11_redeclared(): int { return 41; }');
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11-redeclare.php',
    [],
    ['wave' => 11, 'function' => 'w11_redeclare_error'],
);
printf(
    "status=%s vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
);

$recovered = native_mir_test_compile_execute(
    <<<'PHP'
<?php
function w11_after_redeclare_error(): int {
    return eval('return 42;');
}
PHP,
    'w11-after-redeclare.php',
    [],
    ['wave' => 11, 'function' => 'w11_after_redeclare_error'],
);
printf(
    "recovered=%s return=%d vm=%d execute_ex=%d handler=%d\n",
    $recovered['status'],
    $recovered['execution']['return_value'] ?? -1,
    $recovered['execution']['vm_handler_calls'] ?? -1,
    $recovered['execution']['execute_ex_calls'] ?? -1,
    $recovered['execution']['opline_handler_calls'] ?? -1,
);

$directory = sys_get_temp_dir() . '/native-w11-redeclare-' . getmypid();
mkdir($directory);
$included = $directory . '/redeclare.php';
file_put_contents(
    $included,
    "<?php function w11_include_redeclared(): int { return 41; }\n",
);
file_put_contents($directory . '/recovered.php', "<?php return 42;\n");

$includeResult = native_mir_test_compile_execute(
    <<<'PHP'
<?php
function w11_include_redeclared(): int {
    return 42;
}
function w11_include_redeclare_error(string $path): void {
    include $path;
}
PHP,
    'w11-include-redeclare.php',
    [$included],
    ['wave' => 11, 'function' => 'w11_include_redeclare_error'],
);
printf(
    "include_status=%s vm=%d execute_ex=%d handler=%d\n",
    $includeResult['status'],
    $includeResult['execution']['vm_handler_calls'] ?? -1,
    $includeResult['execution']['execute_ex_calls'] ?? -1,
    $includeResult['execution']['opline_handler_calls'] ?? -1,
);

$includeRecovered = native_mir_test_compile_execute(
    <<<'PHP'
<?php
function w11_after_include_redeclare_error(): int {
    return include __DIR__ . '/recovered.php';
}
PHP,
    $directory . '/entry.php',
    [],
    ['wave' => 11, 'function' => 'w11_after_include_redeclare_error'],
);
printf(
    "include_recovered=%s return=%d vm=%d execute_ex=%d handler=%d\n",
    $includeRecovered['status'],
    $includeRecovered['execution']['return_value'] ?? -1,
    $includeRecovered['execution']['vm_handler_calls'] ?? -1,
    $includeRecovered['execution']['execute_ex_calls'] ?? -1,
    $includeRecovered['execution']['opline_handler_calls'] ?? -1,
);

unlink($included);
@unlink($directory . '/recovered.php');
rmdir($directory);
?>
--EXPECTF--

Fatal error: Cannot redeclare function w11_redeclared() (previously declared in %s) in %s on line 1
status=error vm=0 execute_ex=0 handler=0
recovered=accepted return=42 vm=0 execute_ex=0 handler=0

Fatal error: Cannot redeclare function w11_include_redeclared() (previously declared in %s) in %s on line 1
include_status=error vm=0 execute_ex=0 handler=0
include_recovered=accepted return=42 vm=0 execute_ex=0 handler=0
