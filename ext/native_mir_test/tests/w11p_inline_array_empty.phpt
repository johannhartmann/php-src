--TEST--
Native AArch64 evaluates packed and mixed array empty checks directly
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
function inline_array_empty(
    array $packed,
    array $mixed,
    string $same,
    string $equal,
    string $missing,
): array {
    return [
        empty($packed[0]),
        empty($packed[1]),
        empty($packed[2]),
        empty($packed[3]),
        empty($packed[4]),
        empty($packed[5]),
        empty($packed[6]),
        empty($packed[7]),
        empty($packed[99]),
        empty($mixed[$same]),
        empty($mixed[$equal]),
        empty($mixed[$missing]),
        empty($mixed['null']),
        empty($mixed['reference']),
    ];
}
PHP;

$null = null;
$value = 42;
$same = implode('', ['pre', 'sent']);
$equal = implode('', ['pre', 'sent']);
$packed = [0, 7, '', '0', 'native', [], [1], &$null];
$mixed = [
    $same => 0,
    'null' => null,
    'reference' => &$value,
];
$result = native_mir_test_compile_execute(
    $source,
    'w11p-inline-array-empty.php',
    [$packed, $mixed, $same, $equal, 'missing'],
    [
        'wave' => 11,
        'function' => 'inline_array_empty',
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
accepted return=[true,false,true,true,false,true,false,true,true,true,true,true,true,false] runs=10 vm=0 execute_ex=0 handler=0 active=0
