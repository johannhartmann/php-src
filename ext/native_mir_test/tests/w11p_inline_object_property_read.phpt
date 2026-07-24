--TEST--
Native baseline reads cached standard object properties directly
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
class InlinePropertyBox
{
    public mixed $value;

    public function read()
    {
        return $this->value;
    }
}

function inline_property_value($box)
{
    return $box->value;
}

function inline_property_reads()
{
    $box = new InlinePropertyBox();
    $box->value = "native";
    $string = inline_property_value($box);
    $box->value = [1, 2, 3];
    $array = inline_property_value($box);
    $box->value = 42;
    return [$string, $array, inline_property_value($box), $box->read()];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-inline-object-property-read.php',
    [],
    [
        'wave' => 11,
        'function' => 'inline_property_reads',
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
accepted return=["native",[1,2,3],42,42] vm=0 execute_ex=0 handler=0 active=0
