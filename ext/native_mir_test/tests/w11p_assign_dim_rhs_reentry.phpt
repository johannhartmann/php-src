--TEST--
Native append abandons a replaced destination after an undefined RHS warning
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
function w11p_assign_dim_rhs_reentry(): mixed
{
    set_error_handler(function (int $severity, string $message) use (&$value): bool {
        echo $message, PHP_EOL;
        $value = 0;
        return true;
    });

    $value[] = $undefined;
    restore_error_handler();
    return $value;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-assign-dim-rhs-reentry.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_assign_dim_rhs_reentry',
        'repeat' => 1,
    ],
);

printf(
    "%s return=%s runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
Undefined variable $undefined
accepted return=0 runs=1 vm=0 execute_ex=0 handler=0 active=0
