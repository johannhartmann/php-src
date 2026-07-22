--TEST--
Native MIR W09 executes complete zval operators, casts, and destructuring
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$cases = [
    'numeric_strings' => [<<<'PHP'
<?php
function value_case($integer, $decimal, $dividend, $power)
{
    return [$integer + 2, $decimal * 2, $dividend / 2, $integer % 4, $power ** 5];
}
PHP,
        ["40", "6.5", "9", "2"],
    ],
    'bitwise_strings' => [<<<'PHP'
<?php
function value_case($upper, $lower, $space, $zero, $stringFalse)
{
    return [$upper | $lower, $lower & $upper, $upper ^ $space, ~$zero, !$stringFalse, true xor false];
}
PHP,
        ["A", "a", " ", 0, "0"],
    ],
    'comparisons' => [<<<'PHP'
<?php
function value_case($left, $same, $other)
{
    return [
        $left === $same,
        $left == $same,
        $left != $other,
        $left < $other,
        $left <=> $other,
        "10" == 10,
        "10" < "2",
    ];
}
PHP,
        [['a' => 1, 'b' => 2], ['a' => 1, 'b' => 2], ['a' => 1, 'b' => 3]],
    ],
    'comparison_branch' => [<<<'PHP'
<?php
function value_case($left, $right)
{
    if ($left < $right) {
        return "less";
    }
    return "other";
}
PHP,
        [['a' => 1], ['a' => 2]],
    ],
    'cast_branch' => [<<<'PHP'
<?php
function value_case($value)
{
    if ((bool) $value) {
        return "truthy";
    }
    return "falsey";
}
PHP,
        [[0]],
    ],
    'casts' => [<<<'PHP'
<?php
function value_case($integer, $decimal, $number, $item)
{
    return [(int) $integer, (float) $decimal, (string) $number, (array) $item];
}
PHP,
        ["42", "1.5", 123, "x"],
    ],
    'array_union' => [<<<'PHP'
<?php
function value_case($left, $right)
{
    return $left + $right;
}
PHP,
        [[0 => "left", "shared" => 1], [0 => "right", 1 => "tail", "shared" => 2]],
    ],
    'short_circuit' => [<<<'PHP'
<?php
function value_case($left, $right)
{
    $state = 0;
    $and = $left && ($state = 1);
    $afterAnd = $state;
    $or = $right || ($state = 2);
    return [$and, $afterAnd, $or, $state, "x" && "0", "0" || 5];
}
PHP,
        [false, true],
    ],
    'numeric_type_error' => [<<<'PHP'
<?php
function value_case($invalid)
{
    try {
        return $invalid + 1;
    } catch (TypeError) {
        return "TypeError";
    }
}
PHP,
        ["not numeric"],
    ],
    'division_by_zero' => [<<<'PHP'
<?php
function value_case()
{
    try {
        return 1 / 0;
    } catch (DivisionByZeroError) {
        return "DivisionByZeroError";
    }
}
PHP,
        [],
    ],
    'isset_empty' => [<<<'PHP'
<?php
function value_case()
{
    $null = null;
    $zero = "0";
    return [isset($missing), isset($null), isset($zero), empty($missing), empty($zero)];
}
PHP,
        [],
    ],
    'nested_destructure' => [<<<'PHP'
<?php
function value_case($input)
{
    [$a, [$b, $c]] = $input;
    return [$a, $b, $c];
}
PHP,
        [[1, [2, 3]]],
    ],
    'reference_destructure' => [<<<'PHP'
<?php
function value_case($input)
{
    [&$alias] = $input;
    $alias = 9;
    return $input;
}
PHP,
        [[1]],
    ],
    'cyclic_array_cow' => [<<<'PHP'
<?php
function value_case()
{
    $array = [];
    $array["self"] =& $array;
    $array["value"] = 1;
    $copy = $array;
    $array["value"] = 2;
    return [$array["self"]["value"], $copy["value"], $array === $array["self"]];
}
PHP,
        [],
    ],
];

foreach ($cases as $name => [$source, $arguments]) {
    $result = native_mir_test_compile_execute(
        $source,
        "w09-$name.php",
        $arguments,
        ['wave' => 9, 'function' => 'value_case'],
    );
    printf(
        "%s %s %s return=%s vm=%d execute_ex=%d handler=%d\n",
        $name,
        $result['status'],
        $result['execution']['status'] ?? '-',
        json_encode($result['execution']['return_value'] ?? null),
        $result['execution']['vm_handler_calls'] ?? -1,
        $result['execution']['execute_ex_calls'] ?? -1,
        $result['execution']['opline_handler_calls'] ?? -1,
    );
}
?>
--EXPECT--
numeric_strings accepted returned return=[42,13,4.5,0,32] vm=0 execute_ex=0 handler=0
bitwise_strings accepted returned return=["a","A","a",-1,true,true] vm=0 execute_ex=0 handler=0
comparisons accepted returned return=[true,true,true,true,-1,true,false] vm=0 execute_ex=0 handler=0
comparison_branch accepted returned return="less" vm=0 execute_ex=0 handler=0
cast_branch accepted returned return="truthy" vm=0 execute_ex=0 handler=0
casts accepted returned return=[42,1.5,"123",["x"]] vm=0 execute_ex=0 handler=0
array_union accepted returned return={"0":"left","shared":1,"1":"tail"} vm=0 execute_ex=0 handler=0
short_circuit accepted returned return=[false,0,true,0,false,true] vm=0 execute_ex=0 handler=0
numeric_type_error accepted returned return="TypeError" vm=0 execute_ex=0 handler=0
division_by_zero accepted returned return="DivisionByZeroError" vm=0 execute_ex=0 handler=0
isset_empty accepted returned return=[false,false,true,true,true] vm=0 execute_ex=0 handler=0
nested_destructure accepted returned return=[1,2,3] vm=0 execute_ex=0 handler=0
reference_destructure accepted returned return=[9] vm=0 execute_ex=0 handler=0
cyclic_array_cow accepted returned return=[2,1,true] vm=0 execute_ex=0 handler=0
