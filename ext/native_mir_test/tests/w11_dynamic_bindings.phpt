--TEST--
Native MIR W11 executes indirect variables, globals and dynamic symbol-table operations
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
function w11_dynamic_read(string $name): int {
    $value = 41;
    return $$name + 1;
}
function w11_dynamic_write(string $name): int {
    $$name = 40;
    $$name++;
    return $created + 1;
}
function w11_dynamic_state(string $name): array {
    $value = 1;
    $before = [isset($$name), empty($$name)];
    unset($$name);
    return [$before, isset($value), empty($value)];
}
function w11_global_binding(): int {
    global $w11_shared;
    $w11_shared += 2;
    return $w11_shared;
}
function w11_globals_array(): int {
    return $GLOBALS['w11_shared'];
}
PHP;

$w11_shared = 40;
$cases = [
    ['w11_dynamic_read', ['value']],
    ['w11_dynamic_write', ['created']],
    ['w11_dynamic_state', ['value']],
    ['w11_global_binding', []],
    ['w11_globals_array', []],
];

foreach ($cases as [$function, $arguments]) {
    $result = native_mir_test_compile_execute(
        $source,
        'w11-dynamic-bindings.php',
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
?>
--EXPECT--
w11_dynamic_read accepted return=42 vm=0 execute_ex=0 handler=0
w11_dynamic_write accepted return=42 vm=0 execute_ex=0 handler=0
w11_dynamic_state accepted return=[[true,false],false,true] vm=0 execute_ex=0 handler=0
w11_global_binding accepted return=42 vm=0 execute_ex=0 handler=0
w11_globals_array accepted return=42 vm=0 execute_ex=0 handler=0
