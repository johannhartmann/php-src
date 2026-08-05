--TEST--
Native direct calls transfer array-dimension arguments through their values
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
function w12_direct_indirect_by_reference(&$value): int
{
    $before = $value;
    $value += 4;
    return $before;
}

function w12_direct_indirect_by_value($value): int
{
    $value += 10;
    return $value;
}

function w12_direct_indirect_argument_transfer(): array
{
    $values = [3, 7];
    $before = w12_direct_indirect_by_reference($values[0]);
    $copy = w12_direct_indirect_by_value($values[1]);
    return [$before, $copy, $values];
}
PHP,
    'w12-direct-indirect-argument-transfer.php',
    [],
    [
        'wave' => 11,
        'function' => 'w12_direct_indirect_argument_transfer',
        'repeat' => 20,
    ],
);

printf(
    "%s return=%s codeunits=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['native_codeunits'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=[3,17,[7,7]] codeunits=3 vm=0 execute_ex=0 handler=0 active=0
