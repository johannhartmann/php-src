--TEST--
Native writable property fetch preserves values from temporary receivers
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
final class W11PTemporaryPropertyAddress
{
    public function objectFor(array $values): object
    {
        $object = new stdClass();
        $object->values = $values;
        return $object;
    }
}

function w11p_temporary_object_property_address(): array
{
    $values = [1];
    $extra = $values;

    (new W11PTemporaryPropertyAddress())
        ->objectFor($values)
        ->values[0] = 'changed';

    return [$values, $extra];
}
PHP,
    'w11p-temporary-object-property-address.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_temporary_object_property_address',
        'repeat' => 20,
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
accepted return=[[1],[1]] runs=20 vm=0 execute_ex=0 handler=0 active=0
