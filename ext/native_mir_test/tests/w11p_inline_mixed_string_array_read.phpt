--TEST--
Native baseline reads mixed arrays through guarded string buckets
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
function mixed_string_array_read(array $values, string $first, string $second): array
{
    return [$values[$first], $values[$second]];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-inline-mixed-string-array-read.php',
    [
        ['answer' => 42, 'message' => 'native'],
        'answer',
        'message',
    ],
    [
        'wave' => 11,
        'function' => 'mixed_string_array_read',
        'repeat' => 10,
    ],
);
printf(
    "%s return=%s runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=[42,"native"] runs=10 vm=0 execute_ex=0 handler=0 active=0
