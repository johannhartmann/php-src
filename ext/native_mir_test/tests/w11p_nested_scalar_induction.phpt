--TEST--
Native scalar induction remains exact across a nested branch merge
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
function w11p_nested_scalar_induction(int $n, bool $left): int
{
    $total = 0;
    for ($i = 0; $i < $n; $i++) {
        if ($left) {
            $total++;
        } else {
            $total += 2;
        }
    }
    return $total;
}

function w11p_nested_scalar_induction_root(): array
{
    return [
        w11p_nested_scalar_induction(-1, true),
        w11p_nested_scalar_induction(0, false),
        w11p_nested_scalar_induction(5, true),
        w11p_nested_scalar_induction(5, false),
    ];
}
PHP;

$dump = native_mir_test_compile_dump(
    $source,
    'w11p-nested-scalar-induction.php',
    ['wave' => 11, 'function' => 'w11p_nested_scalar_induction'],
);
$result = native_mir_test_compile_execute(
    $source,
    'w11p-nested-scalar-induction.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_nested_scalar_induction_root',
        'repeat' => 20,
    ],
);

printf(
    "%s scalar-loop=%s return=%s vm=%d\n",
    $result['status'],
    substr_count($dump['mir'], 'opcode i64_lt ') === 1
        && !str_contains($dump['mir'], 'opcode value_binary_op ')
        ? 'yes'
        : 'no',
    json_encode($result['execution']['return_value']),
    $result['execution']['vm_handler_calls'],
);
?>
--EXPECT--
accepted scalar-loop=yes return=[0,0,5,10] vm=0
