--TEST--
Native baseline exposes zero-cost structural and phase performance metrics
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
function metrics_leaf(int $value): int
{
    return $value + 1;
}

function metrics_root(int $count): int
{
    $value = 0;
    for ($index = 0; $index < $count; $index++) {
        $value = metrics_leaf($value);
    }
    return $value;
}
PHP,
    'w11p-performance-metrics.php',
    [100],
    [
        'wave' => 11,
        'function' => 'metrics_root',
        'repeat' => 10,
    ],
);

$performance = $result['execution']['performance'];
printf(
    "%s return=%d executions=%d registered=%d compiled=%d direct=%d "
    . "decode=%d helper=%d heap=%d catcher=%d "
    . "compile=%s execute=%s bytes=%s\n",
    $result['status'],
    $result['execution']['return_value'],
    $performance['executions'],
    $performance['registered_codeunits'],
    $performance['compiled_codeunits'],
    $performance['direct_call_sites'],
    $performance['source_opline_decode_sites'],
    $performance['inner_call_runtime_helper_calls'],
    $performance['inner_call_heap_allocations'],
    $performance['inner_call_catcher_boundaries'],
    $performance['compile_ns'] > 0 ? 'yes' : 'no',
    $performance['execute_ns'] > 0 ? 'yes' : 'no',
    $performance['native_code_bytes'] > 0 ? 'yes' : 'no',
);
?>
--EXPECT--
accepted return=100 executions=10 registered=3 compiled=2 direct=1 decode=0 helper=0 heap=0 catcher=0 compile=yes execute=yes bytes=yes
