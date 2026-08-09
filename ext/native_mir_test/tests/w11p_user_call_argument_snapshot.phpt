--TEST--
Native user calls preserve by-value argument snapshots across later writes
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
function w11p_snapshot_leaf(int $first, int &$middle, int $last): int
{
    $result = $first * 100 + $middle * 10 + $last;
    $middle = 2;
    return $result;
}

function w11p_snapshot_root(): int
{
    $value = 0;
    return w11p_snapshot_leaf($value, $value, $value = 1) * 10 + $value;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-user-call-argument-snapshot.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_snapshot_root',
        'repeat' => 10,
    ],
);
$performance = $result['execution']['performance'];
printf(
    "%s return=%d runs=%d codeunits=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['native_codeunits'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=112 runs=10 codeunits=2 vm=0 execute_ex=0 handler=0 active=0
