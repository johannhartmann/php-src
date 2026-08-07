--TEST--
Native boxed property arrays compose with dimension consumers
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
final class PropertyArrayConsumerBox
{
    public mixed $values = [
        'present' => 'tpde',
        'zero' => 0,
        'null' => null,
    ];
}

function property_array_consumers(array $keys): array
{
    $box = new PropertyArrayConsumerBox();
    $present = $keys[0];
    $zero = $keys[1];
    $missing = $keys[2];
    $null = $keys[3];

    return [
        $box->values[$present],
        isset($box->values[$present]),
        empty($box->values[$zero]),
        isset($box->values[$missing]),
        empty($box->values[$missing]),
        isset($box->values[$null]),
        empty($box->values[$null]),
    ];
}
PHP,
    'w11p-inline-property-array-consumers.php',
    [['present', 'zero', 'missing', 'null']],
    [
        'wave' => 11,
        'function' => 'property_array_consumers',
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
accepted return=["tpde",true,true,false,true,false,true] runs=30 vm=0 execute_ex=0 handler=0 active=0
