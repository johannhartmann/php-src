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
final class NativeAssignmentObject {
    public int $value = 0;
}
function native_object_assign_ref(): int {
    $object = new NativeAssignmentObject();
    $value = 40;
    $object->value =& $value;
    $value += 2;
    return $object->value;
}
function native_object_assign_op(): int {
    $object = new NativeAssignmentObject();
    $object->value = 40;
    $object->value += 2;
    return $object->value;
}
function native_object_assign_dynamic(): int {
    $object = new stdClass();
    $property = 'answer';
    $object->$property = 42;
    return $object->answer;
}
PHP;

foreach ([
    'native_object_assign_ref',
    'native_object_assign_op',
    'native_object_assign_dynamic',
] as $function) {
    $result = native_mir_test_compile_execute(
        $source,
        'explicit-object-assignment-operands.php',
        [],
        ['function' => $function],
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
native_object_assign_ref accepted return=42 vm=0 execute_ex=0 handler=0
native_object_assign_op accepted return=42 vm=0 execute_ex=0 handler=0
native_object_assign_dynamic accepted return=42 vm=0 execute_ex=0 handler=0
