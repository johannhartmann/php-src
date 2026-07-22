--TEST--
Native MIR W09 executes array and Traversable foreach semantics
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$cases = [
    'array_value' => [
        <<<'PHP'
<?php
function foreach_case($values)
{
    $sum = 0;
    foreach ($values as $key => $value) {
        $sum += $value;
    }
    return [$sum, $key];
}
PHP,
        [[2, 3, 5]],
    ],
    'array_reference_cow' => [
        <<<'PHP'
<?php
function foreach_case($values)
{
    $original = $values;
    foreach ($values as $key => &$value) {
        $value *= 2;
    }
    unset($value);
    return [$values, $original, $key];
}
PHP,
        [[2, 3, 5]],
    ],
    'array_mutation' => [
        <<<'PHP'
<?php
function foreach_case($values)
{
    foreach ($values as $value) {
        $values[] = $value;
    }
    return $values;
}
PHP,
        [[2, 3, 5]],
    ],
    'iterator' => [
        <<<'PHP'
<?php
function foreach_case($values)
{
    $result = [];
    foreach ($values as $key => $value) {
        $result[$key] = $value;
    }
    return $result;
}
PHP,
        [new ArrayIterator(['a' => 2, 'b' => 3])],
    ],
    'iterator_aggregate' => [
        <<<'PHP'
<?php
function foreach_case($values)
{
    $result = [];
    foreach ($values as $key => $value) {
        $result[$key] = $value;
    }
    return $result;
}
PHP,
        [new ArrayObject(['a' => 2, 'b' => 3])],
    ],
    'early_return' => [
        <<<'PHP'
<?php
function foreach_case($values)
{
    foreach ($values as $value) {
        return $value;
    }
    return null;
}
PHP,
        [[7, 8]],
    ],
    'empty' => [
        <<<'PHP'
<?php
function foreach_case($values)
{
    foreach ($values as $value) {
        return $value;
    }
    return null;
}
PHP,
        [[]],
    ],
];

foreach ($cases as $name => [$source, $arguments]) {
    $result = native_mir_test_compile_execute(
        $source,
        "w09-foreach-$name.php",
        $arguments,
        ['wave' => 9, 'function' => 'foreach_case'],
    );
    printf(
        "%s %s %s return=%s vm=%d execute_ex=%d handler=%d\n",
        $name,
        $result['status'],
        $result['execution']['status'],
        json_encode($result['execution']['return_value']),
        $result['execution']['vm_handler_calls'],
        $result['execution']['execute_ex_calls'],
        $result['execution']['opline_handler_calls'],
    );
}
?>
--EXPECT--
array_value accepted returned return=[10,2] vm=0 execute_ex=0 handler=0
array_reference_cow accepted returned return=[[4,6,10],[2,3,5],2] vm=0 execute_ex=0 handler=0
array_mutation accepted returned return=[2,3,5,2,3,5] vm=0 execute_ex=0 handler=0
iterator accepted returned return={"a":2,"b":3} vm=0 execute_ex=0 handler=0
iterator_aggregate accepted returned return={"a":2,"b":3} vm=0 execute_ex=0 handler=0
early_return accepted returned return=7 vm=0 execute_ex=0 handler=0
empty accepted returned return=null vm=0 execute_ex=0 handler=0
