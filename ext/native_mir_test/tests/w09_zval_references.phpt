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
    'return_reference' => <<<'PHP'
<?php
function &source_ref(&$value) { return $value; }
function value_case()
{
    $value = "before";
    $alias =& source_ref($value);
    $alias = "after";
    return [$value, $alias];
}
PHP,
    'return_reference_unwrapped' => <<<'PHP'
<?php
function &source_ref(&$value) { return $value; }
function value_case()
{
    $value = "source";
    $copy = source_ref($value);
    $copy = "copy";
    return [$value, $copy];
}
PHP,
    'return_array_dimension_reference' => <<<'PHP'
<?php
function &source_ref(&$value) { return $value['key']; }
function value_case()
{
    $value = ['key' => 1];
    $alias =& source_ref($value);
    $alias = 4;
    return [$value, $alias];
}
PHP,
    'return_reference_chain' => <<<'PHP'
<?php
function &source_ref(&$value) { return $value; }
function &forward_ref(&$value)
{
    $alias =& source_ref($value);
    return $alias;
}
function value_case()
{
    $value = 1;
    $alias =& forward_ref($value);
    $alias = 8;
    return $value;
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
return_reference accepted returned return=["after","after"] vm=0 execute_ex=0 handler=0
return_reference_unwrapped accepted returned return=["source","copy"] vm=0 execute_ex=0 handler=0
return_array_dimension_reference accepted returned return=[{"key":4},4] vm=0 execute_ex=0 handler=0
return_reference_chain accepted returned return=8 vm=0 execute_ex=0 handler=0
