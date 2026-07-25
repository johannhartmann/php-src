--TEST--
Native scalar CVs stay canonical across local variable-variable lookups
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
function w11p_dynamic_local_symbol_table(int $iterations): int
{
    $name = 'value';
    $value = 7;
    $other = 11;
    $sum = 0;
    for ($index = 0; $index < $iterations; $index++) {
        if ($index === 2) {
            $name = 'other';
        }
        $sum += $$name;
        $value++;
        $other += 2;
    }
    return $sum;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-dynamic-local-symbol-table.php',
    [4],
    [
        'wave' => 11,
        'function' => 'w11p_dynamic_local_symbol_table',
        'repeat' => 10,
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
accepted return=47 runs=10 vm=0 execute_ex=0 handler=0 active=0
