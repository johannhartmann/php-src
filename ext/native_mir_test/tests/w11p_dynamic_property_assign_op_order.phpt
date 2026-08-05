--TEST--
Native compound assignment reports dynamic-property creation before undefined read
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
class W11pDynamicPropertyAssignOp
{
}

function w11p_dynamic_property_assign_op_order(): int
{
    $object = new W11pDynamicPropertyAssignOp();
    $object->value += 1;
    return $object->value;
}
PHP,
    'w11p-dynamic-property-assign-op-order.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_dynamic_property_assign_op_order',
    ],
);
printf(
    "%s return=%s vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECTF--
Deprecated: Creation of dynamic property W11pDynamicPropertyAssignOp::$value is deprecated in %s on line %d

Warning: Undefined property: W11pDynamicPropertyAssignOp::$value in %s on line %d
accepted return=1 vm=0 execute_ex=0 handler=0 active=0
