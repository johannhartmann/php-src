--TEST--
Native exceptions unwind unfinished nested direct calls
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
final class W11PNestedCallExceptionUnwind
{
    public function assign(&$value): void
    {
        $value = 'ok';
    }

    public function accept(mixed $value): void
    {
    }

    public function trigger(): void
    {
        $values = [];
        $this->assign($values[]);
        $this->accept($values[]);
    }
}

function w11p_nested_call_exception_unwind(): string
{
    try {
        (new W11PNestedCallExceptionUnwind())->trigger();
    } catch (Error $exception) {
        return $exception->getMessage();
    }

    return 'missed';
}
PHP,
    'w11p-nested-call-exception-unwind.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_nested_call_exception_unwind',
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
accepted return="Cannot use [] for reading" runs=20 vm=0 execute_ex=0 handler=0 active=0
