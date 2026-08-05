--TEST--
Native reused dynamic callables preserve finally exception routing
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
function w11p_dynamic_finally_throw(string $message): void
{
    throw new RuntimeException($message);
}

function w11p_reused_dynamic_callable_finally_route(): array
{
    $thrower = w11p_dynamic_finally_throw(...);

    try {
        try {
            $thrower('try');
        } catch (RuntimeException) {
            $thrower('catch');
        } finally {
            $thrower('finally');
        }
    } catch (RuntimeException $exception) {
        $messages = [];
        do {
            $messages[] = $exception->getMessage();
        } while ($exception = $exception->getPrevious());
        return $messages;
    }

    return [];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-reused-dynamic-callable-finally-route.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_reused_dynamic_callable_finally_route',
        'repeat' => 10,
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
accepted return=["finally","catch"] runs=10 vm=0 execute_ex=0 handler=0 active=0
