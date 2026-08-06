--TEST--
Native untyped ternary PHI drops nonzero after its integer range spans zero
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
function w11p_untyped_ternary($condition)
{
    return $condition ? 1 : -1;
}
PHP;

foreach ([true, false] as $condition) {
    $result = native_mir_test_compile_execute(
        $source,
        'w11p-untyped-ternary-range-union.php',
        [$condition],
        [
            'wave' => 11,
            'function' => 'w11p_untyped_ternary',
            'repeat' => 10,
        ],
    );

    printf(
        "%s return=%s runs=%d active=%d\n",
        $result['status'],
        json_encode($result['execution']['return_value']),
        $result['execution']['executions'],
        $result['execution']['entry_active_calls'],
    );
}
?>
--EXPECT--
accepted return=1 runs=10 active=0
accepted return=-1 runs=10 active=0
