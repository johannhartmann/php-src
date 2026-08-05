--TEST--
Native partial applications source mixed scalar and placeholder arguments from canonical zvals
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
function w11p_mixed_partial_target(
    int $a,
    int $b,
    int $c,
    int $d,
): int {
    return $a + $b + $c + $d;
}

function w11p_mixed_partial_arguments(): int
{
    $a = 1;
    $c = 3;
    $d = 4;
    $partial = w11p_mixed_partial_target($a, ?, $c, $d);

    return $partial(2);
}
PHP,
    'w11p-mixed-partial-arguments.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_mixed_partial_arguments',
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
accepted return=10 closure=ready active=0
