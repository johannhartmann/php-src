--TEST--
Native direct internal call failures release extra named arguments
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
function w11p_direct_internal_extra_named_cleanup(): int
{
    $caught = 0;
    try {
        array_merge([1, 2], extra: [3, 4]);
    } catch (ArgumentCountError) {
        $caught++;
    }
    try {
        var_dump(extra: 0);
    } catch (ArgumentCountError) {
        $caught++;
    }
    return $caught;
}
PHP,
    'w11p-direct-internal-extra-named-cleanup.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_direct_internal_extra_named_cleanup',
        'repeat' => 20,
    ],
);

printf(
    "%s return=%d runs=%d vm=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=2 runs=20 vm=0 active=0
