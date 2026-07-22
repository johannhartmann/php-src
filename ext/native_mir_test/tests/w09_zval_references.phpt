--TEST--
Native MIR W09 executes canonical zval assignment, references, and unset
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$cases = [
    'alias_int' => <<<'PHP'
<?php
function value_case()
{
    $left = 1;
    $right =& $left;
    $right = 2;
    return $left;
}
PHP,
    'alias_string' => <<<'PHP'
<?php
function value_case()
{
    $left = "left";
    $right =& $left;
    $right = "right";
    return $left;
}
PHP,
    'unset_cv' => <<<'PHP'
<?php
function value_case()
{
    $value = "released";
    unset($value);
    return null;
}
PHP,
];

foreach ($cases as $name => $source) {
    $result = native_mir_test_compile_execute(
        $source,
        "w09-$name.php",
        [],
        ['wave' => 9, 'function' => 'value_case'],
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
alias_int accepted returned return=2 vm=0 execute_ex=0 handler=0
alias_string accepted returned return="right" vm=0 execute_ex=0 handler=0
unset_cv accepted returned return=null vm=0 execute_ex=0 handler=0
