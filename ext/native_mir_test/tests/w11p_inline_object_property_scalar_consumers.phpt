--TEST--
Native boxed scalar property reads compose with conditions and arithmetic
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
final class ScalarPropertyConsumerBox
{
    public int $count = 40;
    public bool $enabled = true;
}

function scalar_property_consumers(): int
{
    $box = new ScalarPropertyConsumerBox();

    if ($box->enabled) {
        return $box->count + 2;
    }

    return -1;
}
PHP,
    'w11p-inline-object-property-scalar-consumers.php',
    [],
    [
        'wave' => 11,
        'function' => 'scalar_property_consumers',
        'repeat' => 30,
    ],
);
printf(
    "%s return=%d runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=42 runs=30 vm=0 execute_ex=0 handler=0 active=0
