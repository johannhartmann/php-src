--TEST--
Native MIR W09 executes arrays, dimensions, copy-on-write, and unpacking
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$cases = [
    'literal' => [
        <<<'PHP'
<?php
function array_case($first, $second, $name)
{
    return [$first, $second, 'name' => $name];
}
PHP,
        [1, 2, 'native'],
    ],
    'read' => [
        <<<'PHP'
<?php
function array_case($values, $key)
{
    return $values[$key];
}
PHP,
        [['answer' => 42], 'answer'],
    ],
    'packed_read_scalar' => [
        <<<'PHP'
<?php
function array_case($values, $key)
{
    return $values[$key];
}
PHP,
        [[10, 42, 90], 1],
    ],
    'packed_read_refcounted' => [
        <<<'PHP'
<?php
function array_case($values, $key)
{
    return $values[$key];
}
PHP,
        [['first', 'native', 'last'], 1],
    ],
    'packed_isset_present' => [
        <<<'PHP'
<?php
function array_case($values, $key)
{
    return isset($values[$key]);
}
PHP,
        [[10, 20], 1],
    ],
    'packed_isset_missing' => [
        <<<'PHP'
<?php
function array_case($values, $key)
{
    return isset($values[$key]);
}
PHP,
        [[10, 20], 9],
    ],
    'packed_isset_null' => [
        <<<'PHP'
<?php
function array_case($values, $key)
{
    return isset($values[$key]);
}
PHP,
        [[10, null], 1],
    ],
    'copy_on_write' => [
        <<<'PHP'
<?php
function array_case($left)
{
    $right = $left;
    $right['x'] = 2;
    return [$left['x'], $right['x']];
}
PHP,
        [['x' => 1]],
    ],
    'append' => [
        <<<'PHP'
<?php
function array_case($values, $next)
{
    $values[] = $next;
    return $values;
}
PHP,
        [[10], 20],
    ],
    'packed_append_cv' => [
        <<<'PHP'
<?php
function array_case($value)
{
    $values = [10];
    $values[] = $value;
    return $values;
}
PHP,
        ['native'],
    ],
    'packed_append_tmp_result' => [
        <<<'PHP'
<?php
function array_case($value)
{
    $values = [10];
    $assigned = ($values[] = $value . '!');
    return [$values, $assigned];
}
PHP,
        ['native'],
    ],
    'compound' => [
        <<<'PHP'
<?php
function array_case()
{
    $values = ['count' => 7];
    $values['count'] += 5;
    return $values['count'];
}
PHP,
        [],
    ],
    'unset' => [
        <<<'PHP'
<?php
function array_case()
{
    $values = ['keep' => 1, 'drop' => 2];
    unset($values['drop']);
    return $values;
}
PHP,
        [],
    ],
    'isset_empty' => [
        <<<'PHP'
<?php
function array_case($values)
{
    return [isset($values['present']), isset($values['null']),
        empty($values['zero']), empty($values['missing'])];
}
PHP,
        [['present' => 1, 'null' => null, 'zero' => 0]],
    ],
    'nested' => [
        <<<'PHP'
<?php
function array_case()
{
    $values = [];
    $values['outer']['inner'] = 9;
    return $values['outer']['inner'];
}
PHP,
        [],
    ],
    'unpack' => [
        <<<'PHP'
<?php
function array_case($left, $right)
{
    return [...$left, 'name' => 'before', ...$right];
}
PHP,
        [[1, 'name' => 'left'], [2, 'name' => 'right']],
    ],
];

foreach ($cases as $name => [$source, $arguments]) {
    $result = native_mir_test_compile_execute(
        $source,
        "w09-array-$name.php",
        $arguments,
        ['wave' => 9, 'function' => 'array_case'],
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
literal accepted returned return={"0":1,"1":2,"name":"native"} vm=0 execute_ex=0 handler=0
read accepted returned return=42 vm=0 execute_ex=0 handler=0
packed_read_scalar accepted returned return=42 vm=0 execute_ex=0 handler=0
packed_read_refcounted accepted returned return="native" vm=0 execute_ex=0 handler=0
packed_isset_present accepted returned return=true vm=0 execute_ex=0 handler=0
packed_isset_missing accepted returned return=false vm=0 execute_ex=0 handler=0
packed_isset_null accepted returned return=false vm=0 execute_ex=0 handler=0
copy_on_write accepted returned return=[1,2] vm=0 execute_ex=0 handler=0
append accepted returned return=[10,20] vm=0 execute_ex=0 handler=0
packed_append_cv accepted returned return=[10,"native"] vm=0 execute_ex=0 handler=0
packed_append_tmp_result accepted returned return=[[10,"native!"],"native!"] vm=0 execute_ex=0 handler=0
compound accepted returned return=12 vm=0 execute_ex=0 handler=0
unset accepted returned return={"keep":1} vm=0 execute_ex=0 handler=0
isset_empty accepted returned return=[true,false,true,true] vm=0 execute_ex=0 handler=0
nested accepted returned return=9 vm=0 execute_ex=0 handler=0
unpack accepted returned return={"0":1,"name":"right","1":2} vm=0 execute_ex=0 handler=0
