--TEST--
Native MIR W10 executes closures and callable forms without VM dispatch
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
function w10_target(int $v): int { return $v + 1; }
final class W10CallableMethods {
    public static function plus(int $v): int { return $v + 2; }
    public function plusInstance(int $v): int { return $v + 3; }
}
function w10_callback_api(): int {
    $offset = 4;
    return call_user_func(static fn(int $v): int => $v + $offset, 38);
}
function w10_callback_array(): int {
    $offset = 1;
    $values = array_map(static fn(int $v): int => $v + $offset, [40, 41]);
    return $values[1];
}
function w10_capture_ref(): int {
    $value = 40;
    $callback = function () use (&$value): void { $value += 2; };
    $callback();
    return $value;
}
function w10_first_class(): int { $callback = w10_target(...); return $callback(41); }
function w10_first_class_static(): int { $callback = W10CallableMethods::plus(...); return $callback(40); }
function w10_first_class_instance(): int { $callback = (new W10CallableMethods())->plusInstance(...); return $callback(39); }
PHP;

foreach ([
    'w10_callback_api', 'w10_callback_array', 'w10_capture_ref',
    'w10_first_class', 'w10_first_class_static', 'w10_first_class_instance',
] as $function) {
    $result = native_mir_test_compile_execute(
        $source,
        'w10-callables.php',
        [],
        ['wave' => 10, 'function' => $function],
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
w10_callback_api accepted return=42 vm=0 execute_ex=0 handler=0
w10_callback_array accepted return=42 vm=0 execute_ex=0 handler=0
w10_capture_ref accepted return=42 vm=0 execute_ex=0 handler=0
w10_first_class accepted return=42 vm=0 execute_ex=0 handler=0
w10_first_class_static accepted return=42 vm=0 execute_ex=0 handler=0
w10_first_class_instance accepted return=42 vm=0 execute_ex=0 handler=0
