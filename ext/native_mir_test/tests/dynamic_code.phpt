--TEST--
Native MIR compiles and executes eval and include codeunits without VM fallback
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$directory = sys_get_temp_dir() . '/native-' . getmypid();
mkdir($directory);
$included = $directory . '/loaded.php';
$autoloaded = $directory . '/autoloaded.php';
file_put_contents($included, <<<'PHP'
<?php
function native_loaded(): int {
    return 40;
}
return native_loaded() + 2;
PHP);
file_put_contents($autoloaded, <<<'PHP'
<?php
class NativeAutoloaded {
    public function value(): int {
        return 42;
    }
}
PHP);

$cases = [
    [
        <<<'PHP'
<?php
function native_eval_return(): int {
    return eval('return 42;');
}
PHP,
        'native_eval_return',
        [],
    ],
    [
        <<<'PHP'
<?php
function native_eval_declaration(): int {
    eval('function native_eval_child(): int { return 42; }');
    return native_eval_child();
}
PHP,
        'native_eval_declaration',
        [],
    ],
    [
        <<<'PHP'
<?php
function native_eval_scope(): array {
    $value = 40;
    $result = eval('$value += 2; return $value;');
    return [$result, $value];
}
PHP,
        'native_eval_scope',
        [],
    ],
    [
        <<<'PHP'
<?php
function native_eval_internal_call(): int {
    return eval('return strlen("forty-two");') + 33;
}
PHP,
        'native_eval_internal_call',
        [],
    ],
    [
        <<<'PHP'
<?php
function native_nested_eval(): int {
    return eval('return eval(\'return 42;\');');
}
PHP,
        'native_nested_eval',
        [],
    ],
    [
        <<<'PHP'
<?php
function native_eval_closure(): int {
    $closure = eval('return static fn (int $value): int => $value + 2;');
    return $closure(40);
}
PHP,
        'native_eval_closure',
        [],
    ],
    [
        <<<'PHP'
<?php
function native_eval_class(): int {
    eval('class NativeEvalClass { public function value(): int { return 42; } }');
    return (new NativeEvalClass())->value();
}
PHP,
        'native_eval_class',
        [],
    ],
    [
        <<<'PHP'
<?php
function native_include(string $path): array {
    $first = include $path;
    $once = include_once $path;
    return [$first, $once, native_loaded()];
}
PHP,
        'native_include',
        [$included],
    ],
    [
        <<<'PHP'
<?php
function native_autoload(string $path): int {
    $loader = static function (string $class) use ($path): void {
        include $path;
    };
    spl_autoload_register($loader);
    $value = (new NativeAutoloaded())->value();
    spl_autoload_unregister($loader);
    return $value;
}
PHP,
        'native_autoload',
        [$autoloaded],
    ],
];

foreach ($cases as $index => [$source, $function, $arguments]) {
    $result = native_mir_test_compile_execute(
        $source,
        "dynamic-code-$index.php",
        $arguments,
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

unlink($included);
unlink($autoloaded);
rmdir($directory);
?>
--EXPECT--
native_eval_return accepted return=42 vm=0 execute_ex=0 handler=0
native_eval_declaration accepted return=42 vm=0 execute_ex=0 handler=0
native_eval_scope accepted return=[42,42] vm=0 execute_ex=0 handler=0
native_eval_internal_call accepted return=42 vm=0 execute_ex=0 handler=0
native_nested_eval accepted return=42 vm=0 execute_ex=0 handler=0
native_eval_closure accepted return=42 vm=0 execute_ex=0 handler=0
native_eval_class accepted return=42 vm=0 execute_ex=0 handler=0
native_include accepted return=[42,true,40] vm=0 execute_ex=0 handler=0
native_autoload accepted return=42 vm=0 execute_ex=0 handler=0
