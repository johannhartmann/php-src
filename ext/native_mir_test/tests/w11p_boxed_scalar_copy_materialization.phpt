--TEST--
Native boxed scalar aliases materialize through nested-loop control flow
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
function boxed_scalar_copy_materialization(): string
{
    foreach ([1, 2] as $value) {
        for ($index = 0; $index < 1; $index++) {
            continue 2;
        }
        return 'unreachable';
    }

    return 'ok';
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-boxed-scalar-copy-materialization.php',
    [],
    [
        'wave' => 11,
        'function' => 'boxed_scalar_copy_materialization',
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
accepted return="ok" closure=ready active=0
