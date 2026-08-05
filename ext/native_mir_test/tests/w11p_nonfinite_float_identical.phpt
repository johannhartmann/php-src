--TEST--
Native W11 preserves strict comparison semantics for potentially non-finite floats
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
function w11p_nonfinite_float_identical(float $left, float $right): bool
{
    return $left === $right;
}
PHP,
    'w11p-nonfinite-float-identical.php',
    [NAN, NAN],
    [
        'wave' => 11,
        'function' => 'w11p_nonfinite_float_identical',
        'repeat' => 1,
    ],
);
printf(
    "%s return=%s vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'] ? 'true' : 'false',
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=false vm=0 execute_ex=0 handler=0 active=0
