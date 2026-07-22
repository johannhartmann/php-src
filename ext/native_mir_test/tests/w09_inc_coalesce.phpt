--TEST--
Native MIR W09 executes increment, decrement, coalescing, and Elvis branches
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$cases = [
    'increment_decrement' => [<<<'PHP'
<?php
function value_case($initial)
{
    $value = $initial;
    $postIncrement = $value++;
    $preIncrement = ++$value;
    $postDecrement = $value--;
    $preDecrement = --$value;
    return [$postIncrement, $preIncrement, $postDecrement, $preDecrement, $value];
}
PHP,
        [1],
    ],
    'reference_increment' => [<<<'PHP'
<?php
function value_case()
{
    $value = 1;
    $alias =& $value;
    return [[$alias++, ++$alias, $alias], $value];
}
PHP,
        [],
    ],
    'coalesce_cv' => [<<<'PHP'
<?php
function value_case($present, $null)
{
    return [$present ?? "fallback", $null ?? "fallback", $missing ?? "fallback"];
}
PHP,
        ["value", null],
    ],
    'coalesce_dimension' => [<<<'PHP'
<?php
function value_case($values)
{
    return [
        $values["present"] ?? "fallback",
        $values["null"] ?? "fallback",
        $values["missing"] ?? "fallback",
    ];
}
PHP,
        [["present" => [1, 2], "null" => null]],
    ],
    'elvis' => [<<<'PHP'
<?php
function value_case($null, $false, $zero, $empty, $zeroString, $text, $array)
{
    return [
        $null ?: "fallback",
        $false ?: "fallback",
        $zero ?: "fallback",
        $empty ?: "fallback",
        $zeroString ?: "fallback",
        $text ?: "fallback",
        $array ?: "fallback",
    ];
}
PHP,
        [null, false, 0, "", "0", "value", [1, 2]],
    ],
];

foreach ($cases as $name => [$source, $arguments]) {
    $result = native_mir_test_compile_execute(
        $source,
        "w09-$name.php",
        $arguments,
        ['wave' => 9, 'function' => 'value_case', 'repeat' => 10],
    );
    printf(
        "%s %s %s return=%s runs=%d vm=%d execute_ex=%d handler=%d\n",
        $name,
        $result['status'],
        $result['execution']['status'] ?? '-',
        json_encode($result['execution']['return_value'] ?? null),
        $result['execution']['executions'] ?? -1,
        $result['execution']['vm_handler_calls'] ?? -1,
        $result['execution']['execute_ex_calls'] ?? -1,
        $result['execution']['opline_handler_calls'] ?? -1,
    );
}
?>
--EXPECT--
increment_decrement accepted returned return=[1,3,3,1,1] runs=10 vm=0 execute_ex=0 handler=0
reference_increment accepted returned return=[[1,3,3],3] runs=10 vm=0 execute_ex=0 handler=0
coalesce_cv accepted returned return=["value","fallback","fallback"] runs=10 vm=0 execute_ex=0 handler=0
coalesce_dimension accepted returned return=[[1,2],"fallback","fallback"] runs=10 vm=0 execute_ex=0 handler=0
elvis accepted returned return=["fallback","fallback","fallback","fallback","fallback","value",[1,2]] runs=10 vm=0 execute_ex=0 handler=0
