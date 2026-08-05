--TEST--
Native catches release unfinished partial-call setup without freeing the caller
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
function w11p_unfinished_call_target($a, $b): int
{
    return $a + $b;
}

function w11p_unfinished_call_exception_cleanup(): string
{
    $partial = w11p_unfinished_call_target(b: 10, ...);

    try {
        $partial->__invoke(W11P_UNDEFINED_ARGUMENT);
    } catch (Throwable $exception) {
        return $exception::class . ': ' . $exception->getMessage();
    }

    return 'missed';
}
PHP,
    'w11p-unfinished-call-exception-cleanup.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_unfinished_call_exception_cleanup',
        'repeat' => 20,
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
accepted return="Error: Undefined constant \"W11P_UNDEFINED_ARGUMENT\"" runs=20 vm=0 execute_ex=0 handler=0 active=0
