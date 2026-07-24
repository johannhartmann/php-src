--TEST--
Native object reference and compound assignments consume explicit operands
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
final class W11PAssignmentObject {
    public int $value = 0;
}
function w11p_object_assign_ref(): int {
    $object = new W11PAssignmentObject();
    $value = 40;
    $object->value =& $value;
    $value += 2;
    return $object->value;
}
function w11p_object_assign_op(): int {
    $object = new W11PAssignmentObject();
    $object->value = 40;
    $object->value += 2;
    return $object->value;
}
PHP;

foreach (['w11p_object_assign_ref', 'w11p_object_assign_op'] as $function) {
    $result = native_mir_test_compile_execute(
        $source,
        'w11p-explicit-object-assignment-operands.php',
        [],
        ['wave' => 11, 'function' => $function],
    );
    printf(
        "%s %s return=%s vm=%d execute_ex=%d handler=%d\n",
        $function,
        $result['status'],
        json_encode($result['execution']['return_value'] ?? null),
        $result['execution']['vm_handler_calls'] ?? -1,
        $result['execution']['execute_ex_calls'] ?? -1,
        $result['execution']['opline_handler_calls'] ?? -1,
    );
}
?>
--EXPECT--
w11p_object_assign_ref accepted return=42 vm=0 execute_ex=0 handler=0
w11p_object_assign_op accepted return=42 vm=0 execute_ex=0 handler=0
