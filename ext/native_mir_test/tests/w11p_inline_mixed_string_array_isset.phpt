--TEST--
Native baseline evaluates mixed string array isset through guarded buckets
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
function mixed_string_array_isset(
    array $values,
    string $same,
    string $equal,
    string $missing,
    string $null,
    string $reference,
): array {
    return [
        isset($values[$same]),
        isset($values[$equal]),
        isset($values[$missing]),
        isset($values[$null]),
        isset($values[$reference]),
    ];
}
PHP;

$same = implode('', ['pre', 'sent']);
$equal = implode('', ['pre', 'sent']);
$null = null;
$values = [
    $same => 42,
    'null' => null,
];
$values['reference'] =& $null;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-inline-mixed-string-array-isset.php',
    [$values, $same, $equal, 'missing', 'null', 'reference'],
    [
        'wave' => 11,
        'function' => 'mixed_string_array_isset',
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
accepted return=[true,true,false,false,false] runs=10 vm=0 execute_ex=0 handler=0 active=0
