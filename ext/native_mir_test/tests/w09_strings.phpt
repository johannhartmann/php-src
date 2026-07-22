--TEST--
Native MIR W09 executes strings, offsets, concat, and interpolation ropes
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$cases = [
    'concat' => [
        <<<'PHP'
<?php
function string_case($left, $right)
{
    return $left . $right;
}
PHP,
        ['left', 'right'],
    ],
    'fast_concat' => [
        <<<'PHP'
<?php
function string_case($value)
{
    return "prefix-$value";
}
PHP,
        ['suffix'],
    ],
    'rope' => [
        <<<'PHP'
<?php
function string_case($left, $right)
{
    return "x{$left}y{$right}z";
}
PHP,
        [12, 'Q'],
    ],
    'offsets' => [
        <<<'PHP'
<?php
function string_case($value)
{
    $alias = $value;
    $read = $value[1];
    $negative = $value[-1];
    $present = isset($value[0]);
    $missing = isset($value[20]);
    $emptyZero = empty($value[0]);
    $emptyMissing = empty($value[20]);
    $assigned = ($value[1] = 'XYZ');
    $value[5] = '!';
    return [
        $read,
        $negative,
        $present,
        $missing,
        $emptyZero,
        $emptyMissing,
        $assigned,
        $value,
        $alias,
    ];
}
PHP,
        ['abc'],
    ],
    'offset_empty_error' => [
        <<<'PHP'
<?php
function string_case()
{
    try {
        $value = 'abc';
        $value[0] = '';
    } catch (Error) {
        return 'empty-error';
    }
    return 'missing-error';
}
PHP,
        [],
    ],
    'offset_type_error' => [
        <<<'PHP'
<?php
function string_case()
{
    try {
        $value = 'abc';
        $value[[]] = 'x';
    } catch (TypeError) {
        return 'type-error';
    }
    return 'missing-error';
}
PHP,
        [],
    ],
    'offset_append_error' => [
        <<<'PHP'
<?php
function string_case()
{
    try {
        $value = 'abc';
        $value[] = 'x';
    } catch (Error) {
        return 'append-error';
    }
    return 'missing-error';
}
PHP,
        [],
    ],
    'offset_compound_error' => [
        <<<'PHP'
<?php
function string_case()
{
    try {
        $value = 'abc';
        $value[0] .= 'x';
    } catch (Error) {
        return 'compound-error';
    }
    return 'missing-error';
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
        ['wave' => 9, 'function' => 'string_case'],
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
concat accepted returned return="leftright" vm=0 execute_ex=0 handler=0
fast_concat accepted returned return="prefix-suffix" vm=0 execute_ex=0 handler=0
rope accepted returned return="x12yQz" vm=0 execute_ex=0 handler=0

Warning: Only the first byte will be assigned to the string offset in w09-offsets.php on line 11
offsets accepted returned return=["b","c",true,false,false,true,"X","aXc  !","abc"] vm=0 execute_ex=0 handler=0
offset_empty_error accepted returned return="empty-error" vm=0 execute_ex=0 handler=0
offset_type_error accepted returned return="type-error" vm=0 execute_ex=0 handler=0
offset_append_error accepted returned return="append-error" vm=0 execute_ex=0 handler=0
offset_compound_error accepted returned return="compound-error" vm=0 execute_ex=0 handler=0
