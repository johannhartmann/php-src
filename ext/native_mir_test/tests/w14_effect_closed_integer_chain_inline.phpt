--TEST--
Native component inlines an effect-closed checked integer chain
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
function w14_integer_chain_leaf(int $value, int $step): int
{
    return 100 - (($value + $step) - 1);
}

function w14_integer_chain_root(int $count): int
{
    $value = 0;
    for ($index = 0; $index < $count; $index++) {
        $value = w14_integer_chain_leaf($value, 2);
    }
    return $value;
}

function w14_integer_chain_overflow_root(int $value): int
{
    return w14_integer_chain_leaf($value, 2);
}
PHP,
    'w14-effect-closed-integer-chain-inline.php',
    [2000],
    [
        'wave' => 11,
        'function' => 'w14_integer_chain_root',
        'repeat' => 20,
    ],
);

try {
    native_mir_test_compile_execute(
        <<<'PHP'
<?php
function w14_integer_chain_leaf(int $value, int $step): int
{
    return 100 - (($value + $step) - 1);
}

function w14_integer_chain_overflow_root(int $value): int
{
    return w14_integer_chain_leaf($value, 2);
}
PHP,
        'w14-effect-closed-integer-chain-overflow.php',
        [PHP_INT_MIN],
        [
            'wave' => 11,
            'function' => 'w14_integer_chain_overflow_root',
        ],
    );
    echo "overflow=missing-error\n";
} catch (TypeError $error) {
    echo "overflow=TypeError\n";
}

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
overflow=TypeError
accepted return=0 runs=20 direct=1 inline=1 typed=0 vm=0 execute_ex=0 handler=0
