--TEST--
Native compare-branch fusion uses the split machine CFG
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
function w14_eight_leaf(
    int $a,
    int $b,
    int $c,
    int $d,
    int $e,
    int $f,
    int $g,
    int $h
): int
{
    return $a + $h;
}

function w14_fused_branch_split_cfg(int $n): int
{
    $values = [];
    for ($i = 0; $i < $n; $i++) {
        $values[] = $i;
    }
    return $n === 0 ? 0 : $values[$n - 1];
}

function w14_effect_closed_materialization(int $n): int
{
    $value = 0;
    for ($i = 0; $i < $n; $i++) {
        $value += w14_eight_leaf(1, 2, 3, 4, 5, 6, 7, 8);
    }
    return $value;
}

function w14_register_packed_read(int $n): int
{
    $values = [1, 2, 3, 4, 5, 6, 7, 8];
    $result = 0;
    for ($index = 0; $index < $n; $index++) {
        $key = $index & 7;
        $result += $values[$key];
    }
    return $result;
}
PHP;

foreach ([0, 500] as $argument) {
    $result = native_mir_test_compile_execute(
        $source,
        'w14-fused-branch-split-cfg.php',
        [$argument],
        [
            'wave' => 11,
            'function' => 'w14_fused_branch_split_cfg',
            'repeat' => 10,
        ],
    );
    $execution = $result['execution'];
    printf(
        "%s n=%d return=%d runs=%d vm=%d execute_ex=%d handler=%d\n",
        $result['status'],
        $argument,
        $execution['return_value'],
        $execution['executions'],
        $execution['vm_handler_calls'],
        $execution['execute_ex_calls'],
        $execution['opline_handler_calls'],
    );
}

$result = native_mir_test_compile_execute(
    $source,
    'w14-effect-closed-materialization.php',
    [2],
    [
        'wave' => 11,
        'function' => 'w14_effect_closed_materialization',
        'repeat' => 10,
    ],
);
$execution = $result['execution'];
printf(
    "%s materialized return=%d runs=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $execution['return_value'],
    $execution['executions'],
    $execution['vm_handler_calls'],
    $execution['execute_ex_calls'],
    $execution['opline_handler_calls'],
);

$result = native_mir_test_compile_execute(
    $source,
    'w14-register-packed-read.php',
    [500],
    [
        'wave' => 11,
        'function' => 'w14_register_packed_read',
        'repeat' => 10,
    ],
);
$execution = $result['execution'];
printf(
    "%s packed return=%d runs=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $execution['return_value'],
    $execution['executions'],
    $execution['vm_handler_calls'],
    $execution['execute_ex_calls'],
    $execution['opline_handler_calls'],
);
?>
--EXPECT--
accepted n=0 return=0 runs=10 vm=0 execute_ex=0 handler=0
accepted n=500 return=499 runs=10 vm=0 execute_ex=0 handler=0
accepted materialized return=18 runs=10 vm=0 execute_ex=0 handler=0
accepted packed return=2242 runs=10 vm=0 execute_ex=0 handler=0
