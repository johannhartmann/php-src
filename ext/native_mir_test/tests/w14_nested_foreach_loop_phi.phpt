--TEST--
Native nested foreach preserves enclosing loop PHIs
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$source = <<<'PHP'
<?php
function nested_foreach_sum(int $count): int
{
    $values = [1, 2, 3, 4];
    $sum = 0;
    for ($index = 0; $index < $count; $index++) {
        foreach ($values as $value) {
            $sum += $value;
        }
    }
    return $sum;
}
PHP;

foreach ([0, 1, 2, 10] as $count) {
    $result = native_mir_test_compile_execute(
        $source,
        "w14-nested-foreach-loop-{$count}.php",
        [$count],
        [
            'wave' => 11,
            'function' => 'nested_foreach_sum',
        ],
    );
    printf(
        "count[%d]=%s vm=%d execute_ex=%d handler=%d\n",
        $count,
        json_encode($result['execution']['return_value']),
        $result['execution']['vm_handler_calls'],
        $result['execution']['execute_ex_calls'],
        $result['execution']['opline_handler_calls'],
    );
}
?>
--EXPECT--
count[0]=0 vm=0 execute_ex=0 handler=0
count[1]=10 vm=0 execute_ex=0 handler=0
count[2]=20 vm=0 execute_ex=0 handler=0
count[10]=100 vm=0 execute_ex=0 handler=0
