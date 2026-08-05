--TEST--
Native scalar assignment expressions materialize their canonical temporary
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
function w11p_assignment_expression_materialization(): array
{
    $name = 'observed';
    $observed = 1;
    $dynamic = $$name;

    $i = $j = 0;
    $j = $i++;

    return [$dynamic, $i, $j];
}
PHP,
    'w11p-assignment-expression-materialization.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_assignment_expression_materialization',
        'repeat' => 20,
    ],
);
printf(
    "%s return=%s closure=%s active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    ($result['execution']['failed_codeunits'] ?? -1) === 0
        && ($result['execution']['performance']['ready_codeunits'] ?? -1)
            === ($result['execution']['performance']['compiled_codeunits'] ?? -2)
        ? 'ready'
        : 'incomplete',
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=[1,1,0] closure=ready active=0
