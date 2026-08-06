--TEST--
Native generator resume preserves a yielded internal-call argument
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
function w12_generator_internal_call_argument(): Generator
{
    $rendered = sprintf('<%s>', yield 'ready');
    yield $rendered;
    return $rendered;
}

function w12_generator_internal_call_argument_root(): array
{
    $generator = w12_generator_internal_call_argument();
    $first = $generator->current();
    $second = $generator->send('sent');
    $generator->next();

    return [$first, $second, $generator->getReturn()];
}
PHP,
    'w12-generator-internal-call-argument.php',
    [],
    [
        'wave' => 11,
        'function' => 'w12_generator_internal_call_argument_root',
        'repeat' => 10,
        'stack_probe' => true,
    ],
);

printf(
    "%s result=%s gateway=%s runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    ($result['execution']['generator_reentry_gateway_calls'] ?? 0) > 0
        ? 'yes' : 'no',
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted result=["ready","<sent>","<sent>"] gateway=yes runs=10 vm=0 execute_ex=0 handler=0 active=0
