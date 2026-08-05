--TEST--
Native baseline resets direct CV array iterators without the runtime helper
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$cases = [
    'repeated' => [
        <<<'PHP'
<?php
function iterator_reset_case($values)
{
    $sum = 0;
    foreach ($values as $value) {
        $sum += $value;
    }
    foreach ($values as $value) {
        $sum += $value;
    }
    return $sum;
}
PHP,
        [[2, 3, 5]],
    ],
    'empty' => [
        <<<'PHP'
<?php
function iterator_reset_case($values)
{
    foreach ($values as $value) {
        return $value;
    }
    return 'empty';
}
PHP,
        [[]],
    ],
    'cow' => [
        <<<'PHP'
<?php
function iterator_reset_case($values)
{
    $original = $values;
    foreach ($values as $value) {
    }
    $values[] = 7;
    return [$values, $original];
}
PHP,
        [[2, 3, 5]],
    ],
    'post-mutation' => [
        <<<'PHP'
<?php
function iterator_reset_case($values)
{
    foreach ($values as $value) {
    }
    $values[] = 7;
    return $values;
}
PHP,
        [[2, 3, 5]],
    ],
    'object-fallback' => [
        <<<'PHP'
<?php
function iterator_reset_case($values)
{
    $sum = 0;
    foreach ($values as $value) {
        $sum += $value;
    }
    return $sum;
}
PHP,
        [new ArrayIterator([2, 3, 5])],
    ],
];

foreach ($cases as $name => [$source, $arguments]) {
    $result = native_mir_test_compile_execute(
        $source,
        "w11p-inline-array-iterator-reset-$name.php",
        $arguments,
        [
            'wave' => 11,
            'function' => 'iterator_reset_case',
            'repeat' => 10,
        ],
    );
    printf(
        "%s %s return=%s vm=%d execute_ex=%d handler=%d active=%d\n",
        $name,
        $result['status'],
        json_encode($result['execution']['return_value']),
        $result['execution']['vm_handler_calls'],
        $result['execution']['execute_ex_calls'],
        $result['execution']['opline_handler_calls'],
        $result['execution']['entry_active_calls'],
    );
}
?>
--EXPECT--
repeated accepted return=20 vm=0 execute_ex=0 handler=0 active=0
empty accepted return="empty" vm=0 execute_ex=0 handler=0 active=0
cow accepted return=[[2,3,5,7],[2,3,5]] vm=0 execute_ex=0 handler=0 active=0
post-mutation accepted return=[2,3,5,7] vm=0 execute_ex=0 handler=0 active=0
object-fallback accepted return=10 vm=0 execute_ex=0 handler=0 active=0
