--TEST--
Native FETCH_DIM_RW preserves exception chaining after an undefined container warning
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
function w11p_fetch_dim_rw_exception_chain(): array
{
    set_error_handler(function (string $message): void {
    });
    $key = [];
    try {
        $value[$key]++;
    } catch (TypeError $exception) {
        restore_error_handler();
        return [
            $exception->getMessage(),
            $exception->getPrevious() instanceof TypeError,
        ];
    }
    restore_error_handler();
    return ['missed', false];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-fetch-dim-rw-exception-chain.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_fetch_dim_rw_exception_chain',
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
accepted return=["Cannot access offset of type array on array",false] runs=1 vm=0 execute_ex=0 handler=0 active=0
