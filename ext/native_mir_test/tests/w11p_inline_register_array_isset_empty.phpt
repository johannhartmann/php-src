--TEST--
Native array isset and empty compose register receivers and keys
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
function register_array_isset_empty(array $nested): array
{
    $values = $nested['values'];
    $keys = $nested['keys'];
    $packed = $nested['packed'];

    $present = $keys[0];
    $zero = $keys[1];
    $missing = $keys[2];
    $null = $keys[3];
    $index = $keys[4];

    return [
        isset($values[$present]),
        empty($values[$zero]),
        isset($values[$missing]),
        empty($values[$missing]),
        isset($values[$null]),
        empty($values[$null]),
        isset($packed[$index]),
        empty($packed[$index]),
    ];
}
PHP,
    'w11p-inline-register-array-isset-empty.php',
    [[
        'values' => [
            'present' => 42,
            'zero' => 0,
            'null' => null,
        ],
        'keys' => ['present', 'zero', 'missing', 'null', 0],
        'packed' => [9],
    ]],
    [
        'wave' => 11,
        'function' => 'register_array_isset_empty',
        'repeat' => 30,
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
accepted return=[true,true,false,true,false,true,true,false] runs=30 vm=0 execute_ex=0 handler=0 active=0
