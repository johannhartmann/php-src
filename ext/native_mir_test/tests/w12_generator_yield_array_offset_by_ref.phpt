--TEST--
Native by-reference generator yield dereferences an indirect array offset
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$result = native_mir_test_compile_execute(
    <<<'PHP'
<?php
function &w12_generator_array_reference(array &$array)
{
    yield $array[0];
}

function w12_generator_yield_array_offset_by_ref_root()
{
    $array = [1, 2, 3];
    foreach (w12_generator_array_reference($array) as &$value) {
        $value *= -1;
    }
    return $array;
}
PHP,
    'w12-generator-yield-array-offset-by-ref.php',
    [],
    [
        'wave' => 11,
        'function' => 'w12_generator_yield_array_offset_by_ref_root',
        'repeat' => 10,
        'stack_probe' => true,
    ],
);

printf(
    "%s result=%s gateway=%s runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    ($result['execution']['generator_reentry_gateway_calls'] ?? 0) > 0
        ? 'yes' : 'no',
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted result=[-1,2,3] gateway=yes runs=10 vm=0 execute_ex=0 handler=0 active=0
