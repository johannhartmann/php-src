--TEST--
Native baseline writes cached standard object properties directly
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
class InlineWriteBox
{
    public $value = 0;
}

function inline_property_writes()
{
    $box = new InlineWriteBox();
    $value = "native";
    $box->value = $value;
    $string = $box->value;
    $value = [1, 2, 3];
    $box->value = $value;
    $array = $box->value;
    $value = 42;
    $box->value = $value;
    return [$string, $array, $box->value];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-inline-object-property-write.php',
    [],
    [
        'wave' => 11,
        'function' => 'inline_property_writes',
        'repeat' => 10,
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
--EXPECT--
accepted return=["native",[1,2,3],42] vm=0 execute_ex=0 handler=0 active=0
