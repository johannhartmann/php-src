--TEST--
Native component inlines an effect-closed scalar diamond
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
function w14_scalar_diamond_leaf(int $value): int
{
    return $value < 0 ? 0 : $value;
}

function w14_scalar_diamond_root(int $start): int
{
    $total = 0;
    for ($value = $start; $value <= 3; $value++) {
        $total += w14_scalar_diamond_leaf($value);
    }
    return $total;
}
PHP,
    'w14-effect-closed-scalar-diamond-inline.php',
    [-3],
    [
        'wave' => 11,
        'function' => 'w14_scalar_diamond_root',
        'repeat' => 20,
    ],
);

$execution = $result['execution'];
$performance = $execution['performance'];
printf(
    "%s return=%d runs=%d direct=%d inline=%d typed=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $execution['return_value'],
    $execution['executions'],
    $performance['direct_call_sites'],
    $performance['direct_leaf_scalar_sites'],
    $performance['direct_typed_body_sites'],
    $execution['vm_handler_calls'],
    $execution['execute_ex_calls'],
    $execution['opline_handler_calls'],
);
?>
--EXPECT--
accepted return=6 runs=20 direct=1 inline=1 typed=0 vm=0 execute_ex=0 handler=0
