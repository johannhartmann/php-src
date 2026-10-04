--TEST--
Native MIR resolves already loaded functions and methods across codeunits
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
class NativeLoadedTarget {
    public function value(int $input): int { return $input + 2; }
    public static function staticValue(int $input): int { return $input + 2; }
}
function native_loaded_function(int $input): int { return $input + 2; }

$source = <<<'PHP'
<?php
function native_call_loaded_function(): int { $name = 'native_loaded_function'; return $name(40); }
function native_call_loaded_method(): int { $class = 'NativeLoadedTarget'; $method = 'value'; return (new $class())->$method(40); }
function native_call_loaded_static(): int { $class = 'NativeLoadedTarget'; $method = 'staticValue'; return $class::$method(40); }
PHP;

foreach (['native_call_loaded_function', 'native_call_loaded_method', 'native_call_loaded_static'] as $function) {
    $result = native_mir_test_compile_execute(
        $source,
        'loaded-codeunits.php',
        [],
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
?>
--EXPECT--
native_call_loaded_function accepted return=42 vm=0 execute_ex=0 handler=0
native_call_loaded_method accepted return=42 vm=0 execute_ex=0 handler=0
native_call_loaded_static accepted return=42 vm=0 execute_ex=0 handler=0
