--TEST--
Native MIR W09 executes concat, fast concat, and interpolation ropes
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
