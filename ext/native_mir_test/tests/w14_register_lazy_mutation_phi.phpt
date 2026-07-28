--TEST--
Native loop mutations remain in boxed TPDE PHIs until observable boundaries
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
class LazyMutationBox
{
    public int $value = 7;
}

function lazy_property_sum(int $count): mixed
{
    $box = new LazyMutationBox();
    $sum = 0;
    for ($index = 0; $index < $count; $index++) {
        $sum += $box->value;
    }
    return $sum;
}

function lazy_overflow_sum(int $count): mixed
{
    $sum = PHP_INT_MAX;
    for ($index = 0; $index < $count; $index++) {
        $sum += 1;
    }
    return $sum;
}

function lazy_increment(int $count): mixed
{
    $value = 0;
    for ($index = 0; $index < $count; $index++) {
        $value++;
    }
    return $value;
}
PHP;

foreach ([0, 1, 500] as $count) {
    $result = native_mir_test_compile_execute(
        $source,
        "w14-register-lazy-property-{$count}.php",
        [$count],
        [
            'wave' => 11,
            'function' => 'lazy_property_sum',
        ],
    );
    printf(
        "property[%d]=%s vm=%d execute_ex=%d handler=%d\n",
        $count,
        json_encode($result['execution']['return_value']),
        $result['execution']['vm_handler_calls'],
        $result['execution']['execute_ex_calls'],
        $result['execution']['opline_handler_calls'],
    );
}

foreach ([0, 1, 2] as $count) {
    $result = native_mir_test_compile_execute(
        $source,
        "w14-register-lazy-overflow-{$count}.php",
        [$count],
        [
            'wave' => 11,
            'function' => 'lazy_overflow_sum',
        ],
    );
    printf(
        "overflow[%d]=%s:%s vm=%d\n",
        $count,
        get_debug_type($result['execution']['return_value']),
        json_encode($result['execution']['return_value']),
        $result['execution']['vm_handler_calls'],
    );
}

foreach ([0, 1, 1000] as $count) {
    $result = native_mir_test_compile_execute(
        $source,
        "w14-register-lazy-increment-{$count}.php",
        [$count],
        [
            'wave' => 11,
            'function' => 'lazy_increment',
        ],
    );
    printf(
        "increment[%d]=%s vm=%d\n",
        $count,
        json_encode($result['execution']['return_value']),
        $result['execution']['vm_handler_calls'],
    );
}
?>
--EXPECT--
property[0]=0 vm=0 execute_ex=0 handler=0
property[1]=7 vm=0 execute_ex=0 handler=0
property[500]=3500 vm=0 execute_ex=0 handler=0
overflow[0]=int:9223372036854775807 vm=0
overflow[1]=float:9.223372036854776e+18 vm=0
overflow[2]=float:9.223372036854776e+18 vm=0
increment[0]=0 vm=0
increment[1]=1 vm=0
increment[1000]=1000 vm=0
