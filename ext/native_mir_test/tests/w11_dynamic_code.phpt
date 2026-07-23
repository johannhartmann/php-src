--TEST--
Native MIR W11 compiles and executes eval and include codeunits without VM fallback
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$directory = sys_get_temp_dir() . '/native-w11-' . getmypid();
mkdir($directory);
$included = $directory . '/loaded.php';
file_put_contents($included, <<<'PHP'
<?php
function w11_loaded(): int {
    return 40;
}
return w11_loaded() + 2;
PHP);

$cases = [
    [
        <<<'PHP'
<?php
function w11_eval_return(): int {
    return eval('return 42;');
}
PHP,
        'w11_eval_return',
        [],
    ],
    [
        <<<'PHP'
<?php
function w11_eval_declaration(): int {
    eval('function w11_eval_child(): int { return 42; }');
    return w11_eval_child();
}
PHP,
        'w11_eval_declaration',
        [],
    ],
    [
        <<<'PHP'
<?php
function w11_eval_scope(): array {
    $value = 40;
    $result = eval('$value += 2; return $value;');
    return [$result, $value];
}
PHP,
        'w11_eval_scope',
        [],
    ],
    [
        <<<'PHP'
<?php
function w11_include(string $path): array {
    $first = include $path;
    $once = include_once $path;
    return [$first, $once, w11_loaded()];
}
PHP,
        'w11_include',
        [$included],
    ],
];

foreach ($cases as $index => [$source, $function, $arguments]) {
    $result = native_mir_test_compile_execute(
        $source,
        "w11-dynamic-code-$index.php",
        $arguments,
        ['wave' => 11, 'function' => $function],
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

unlink($included);
rmdir($directory);
?>
--EXPECT--
w11_eval_return accepted return=42 vm=0 execute_ex=0 handler=0
w11_eval_declaration accepted return=42 vm=0 execute_ex=0 handler=0
w11_eval_scope accepted return=[42,42] vm=0 execute_ex=0 handler=0
w11_include accepted return=[42,true,40] vm=0 execute_ex=0 handler=0
