--TEST--
Native MIR W08 executes direct internal functions and methods
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
function run_w08(
    string $source,
    string $function,
    array $arguments,
    ?Closure $normalize = null,
): void
{
    $result = native_mir_test_compile_execute(
        $source,
        $function . '.php',
        $arguments,
        ['wave' => 8, 'function' => $function],
    );
    $returnValue = $result['execution']['return_value'];
    if ($normalize !== null) {
        $returnValue = $normalize($returnValue);
    }
    printf(
        "%s %s %s return=%s vm=%d execute_ex=%d handler=%d\n",
        $function,
        $result['status'],
        $result['execution']['status'],
        json_encode($returnValue),
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

run_w08(<<<'PHP'
<?php
function byref_internal(string $value, int $count): int
{
    str_replace('a', 'b', $value, $count);
    return strcmp(json_encode($count), '2');
}
PHP, 'byref_internal', ['a-a', 0]);

run_w08(<<<'PHP'
<?php
function null_internal(string $value)
{
    return json_decode($value);
}
PHP, 'null_internal', ['null']);

run_w08(<<<'PHP'
<?php
function bool_internal(array $value)
{
    return array_is_list($value);
}
PHP, 'bool_internal', [[1, 2]]);

run_w08(<<<'PHP'
<?php
function double_internal(float $value)
{
    return deg2rad($value);
}
PHP, 'double_internal', [180.0]);

run_w08(<<<'PHP'
<?php
function string_internal(string $value)
{
    return strrev($value);
}
PHP, 'string_internal', ['abc']);

run_w08(<<<'PHP'
<?php
function array_internal(array $value)
{
    return array_values($value);
}
PHP, 'array_internal', [['x' => 1, 'y' => 2]]);

run_w08(<<<'PHP'
<?php
function static_internal_method(): int
{
    return spl_object_id(DateTime::createFromFormat('Y-m-d', '2020-01-01'));
}
PHP, 'static_internal_method', [], static function (mixed $value): string {
    return is_int($value) && $value > 0 ? 'positive' : 'invalid';
});

run_w08(<<<'PHP'
<?php
function instance_internal_method(WeakMap $map): int
{
    return $map->count();
}
PHP, 'instance_internal_method', [new WeakMap()]);
?>
--EXPECT--
direct_internal accepted returned return=0 vm=0 execute_ex=0 handler=0
nested_internal accepted returned return=0 vm=0 execute_ex=0 handler=0
optional_internal accepted returned return=0 vm=0 execute_ex=0 handler=0
variadic_internal accepted returned return=0 vm=0 execute_ex=0 handler=0
byref_internal accepted returned return=0 vm=0 execute_ex=0 handler=0
null_internal accepted returned return=null vm=0 execute_ex=0 handler=0
bool_internal accepted returned return=true vm=0 execute_ex=0 handler=0
double_internal accepted returned return=3.141592653589793 vm=0 execute_ex=0 handler=0
string_internal accepted returned return="cba" vm=0 execute_ex=0 handler=0
array_internal accepted returned return=[1,2] vm=0 execute_ex=0 handler=0
static_internal_method accepted returned return="positive" vm=0 execute_ex=0 handler=0
instance_internal_method accepted returned return=0 vm=0 execute_ex=0 handler=0
