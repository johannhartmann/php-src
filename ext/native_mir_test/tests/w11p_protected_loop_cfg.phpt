--TEST--
Native protected loop roots preserve valid CFG structure
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
function w11p_protected_loop_cfg(): array
{
    $result = [];
    foreach ([0, 2, 0] as $divisor) {
        try {
            $result[] = intdiv(8, $divisor);
        } catch (DivisionByZeroError $exception) {
            $result[] = 'zero';
        }
    }
    return $result;
}
PHP,
    'w11p-protected-loop-cfg.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_protected_loop_cfg',
        'repeat' => 10,
    ],
);
printf(
    "%s return=%s vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=["zero",4,"zero"] vm=0 execute_ex=0 handler=0 active=0
