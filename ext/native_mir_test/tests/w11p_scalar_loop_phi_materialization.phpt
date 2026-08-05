--TEST--
Native scalar loop PHIs do not materialize undefined boolean registers
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
function scalar_loop_phi_materialization(bool $selectTrue): bool
{
    $passOption = false;
    $repeat = true;

    while ($repeat) {
        $repeat = false;
        $switch = $selectTrue ? '-n' : '--help';

        switch ($switch) {
            case '-n':
                $passOption = true;
                break;
            case '--help':
                echo '';
                return $passOption;
        }
    }

    return $passOption;
}

function scalar_loop_phi_materialization_root(): array
{
    return [
        scalar_loop_phi_materialization(false),
        scalar_loop_phi_materialization(true),
    ];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-scalar-loop-phi-materialization.php',
    [],
    [
        'wave' => 11,
        'function' => 'scalar_loop_phi_materialization_root',
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
accepted return=[false,true] closure=ready active=0
