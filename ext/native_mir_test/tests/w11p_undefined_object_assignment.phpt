--TEST--
Native object assignment evaluates undefined value and property CVs in VM order
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
#[AllowDynamicProperties]
class W11pUndefinedObjectAssignment
{
    public function __set(string $name, mixed $value): void
    {
        $this->$property = $assigned;
    }
}

function w11p_undefined_object_assignment(): string
{
    $object = new W11pUndefinedObjectAssignment();
    $property = '' & '';
    $object->$property = 0;
    return 'Done';
}
PHP,
    'w11p-undefined-object-assignment.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_undefined_object_assignment',
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
Warning: Undefined variable $assigned in %s on line %d

Warning: Undefined variable $property in %s on line %d
accepted return="Done" vm=0 execute_ex=0 handler=0 active=0
