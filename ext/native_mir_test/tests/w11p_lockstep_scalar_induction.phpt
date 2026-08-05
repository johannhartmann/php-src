--TEST--
Native scalar induction proves equal lockstep counters only
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
function w11p_lockstep_scalar_induction(int $n): int
{
    $total = 0;
    for ($i = 0; $i < $n; $i++) {
        $total++;
    }
    return $total;
}

function w11p_lockstep_scalar_overflow(int $n)
{
    $total = PHP_INT_MAX - 1;
    for ($i = 0; $i < $n; $i++) {
        $total++;
    }
    return $total;
}

function w11p_lockstep_scalar_root(): array
{
    $overflow = w11p_lockstep_scalar_overflow(2);
    return [
        w11p_lockstep_scalar_induction(-1),
        w11p_lockstep_scalar_induction(0),
        w11p_lockstep_scalar_induction(5),
        is_float($overflow),
    ];
}
PHP;

$scalar = native_mir_test_compile_dump(
    $source,
    'w11p-lockstep-scalar-induction.php',
    ['wave' => 11, 'function' => 'w11p_lockstep_scalar_induction'],
);
$overflow = native_mir_test_compile_dump(
    $source,
    'w11p-lockstep-scalar-overflow.php',
    ['wave' => 11, 'function' => 'w11p_lockstep_scalar_overflow'],
);
$result = native_mir_test_compile_execute(
    $source,
    'w11p-lockstep-scalar-induction.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_lockstep_scalar_root',
        'repeat' => 20,
    ],
);

printf(
    "%s scalar=%s overflow-guard=%s return=%s vm=%d\n",
    $result['status'],
    str_contains($scalar['mir'], 'opcode i64_add_no_overflow ')
        && !str_contains($scalar['mir'], 'opcode value_incdec ')
        ? 'yes'
        : 'no',
    str_contains($overflow['mir'], 'opcode value_incdec ')
        ? 'yes'
        : 'no',
    json_encode($result['execution']['return_value']),
    $result['execution']['vm_handler_calls'],
);
?>
--EXPECT--
accepted scalar=yes overflow-guard=yes return=[0,0,5,true] vm=0
