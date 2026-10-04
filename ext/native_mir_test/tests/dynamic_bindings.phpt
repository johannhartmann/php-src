--TEST--
Native MIR executes indirect variables, globals and dynamic symbol-table operations
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
function native_dynamic_read(string $name): int {
    $value = 41;
    return $$name + 1;
}
function native_dynamic_write(string $name): int {
    $$name = 40;
    $$name++;
    return $created + 1;
}
function native_dynamic_state(string $name): array {
    $value = 1;
    $before = [isset($$name), empty($$name)];
    unset($$name);
    return [$before, isset($value), empty($value)];
}
function native_global_binding(): int {
    global $native_shared;
    $native_shared += 2;
    return $native_shared;
}
function native_globals_array(): int {
    return $GLOBALS['native_shared'];
}
PHP;

$native_shared = 40;
$cases = [
    ['native_dynamic_read', ['value']],
    ['native_dynamic_write', ['created']],
    ['native_dynamic_state', ['value']],
    ['native_global_binding', []],
    ['native_globals_array', []],
];

foreach ($cases as [$function, $arguments]) {
    $result = native_mir_test_compile_execute(
        $source,
        'dynamic-bindings.php',
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
?>
--EXPECT--
native_dynamic_read accepted return=42 vm=0 execute_ex=0 handler=0
native_dynamic_write accepted return=42 vm=0 execute_ex=0 handler=0
native_dynamic_state accepted return=[[true,false],false,true] vm=0 execute_ex=0 handler=0
native_global_binding accepted return=42 vm=0 execute_ex=0 handler=0
native_globals_array accepted return=42 vm=0 execute_ex=0 handler=0
