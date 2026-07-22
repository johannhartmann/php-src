--TEST--
Native MIR W08 executes direct, nested, optional, and variadic internal calls
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
function run_w08(string $source, string $function, array $arguments): void
{
    $result = native_mir_test_compile_execute(
        $source,
        $function . '.php',
        $arguments,
        ['wave' => 8, 'function' => $function],
    );
    printf(
        "%s %s %s return=%s vm=%d execute_ex=%d handler=%d\n",
        $function,
        $result['status'],
        $result['execution']['status'],
        json_encode($result['execution']['return_value']),
        $result['execution']['vm_handler_calls'],
        $result['execution']['execute_ex_calls'],
        $result['execution']['opline_handler_calls'],
    );
}

run_w08(<<<'PHP'
<?php
function direct_internal(string $value): int
{
    return strcmp($value, 'hello');
}
PHP, 'direct_internal', ['hello']);

run_w08(<<<'PHP'
<?php
function nested_internal(string $value): int
{
    return strcmp(strrev($value), 'cba');
}
PHP, 'nested_internal', ['abc']);

run_w08(<<<'PHP'
<?php
function optional_internal(string $value): int
{
    return strcmp(json_encode($value), '"hello"');
}
PHP, 'optional_internal', ['hello']);

run_w08(<<<'PHP'
<?php
function variadic_internal(string $format, string $value): int
{
    return strcmp(sprintf($format, $value, 2, 'x'), 'a-2-x');
}
PHP, 'variadic_internal', ['%s-%d-%s', 'a']);
?>
--EXPECT--
direct_internal accepted returned return=0 vm=0 execute_ex=0 handler=0
nested_internal accepted returned return=0 vm=0 execute_ex=0 handler=0
optional_internal accepted returned return=0 vm=0 execute_ex=0 handler=0
variadic_internal accepted returned return=0 vm=0 execute_ex=0 handler=0
