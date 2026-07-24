--TEST--
Native baseline copies boxed CV arguments in generated native-to-native frames
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
function inline_boxed_identity($value)
{
    return $value;
}

function inline_boxed_root()
{
    $string = "native";
    $array = [1, 2, 3];
    $object = (object) ["value" => 4];
    $stringResult = inline_boxed_identity($string);
    $arrayResult = inline_boxed_identity($array);
    $objectResult = inline_boxed_identity($object);
    return [
        $string,
        $stringResult,
        $array,
        $arrayResult,
        $object->value,
        $objectResult->value,
    ];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-inline-boxed-native-frames.php',
    [],
    [
        'wave' => 11,
        'function' => 'inline_boxed_root',
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
accepted return=["native","native",[1,2,3],[1,2,3],4,4] vm=0 execute_ex=0 handler=0 active=0
