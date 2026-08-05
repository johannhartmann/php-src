--TEST--
Native generators freeze and restore pending call setup records
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
function w12_pending_calls(array &$trace): Generator
{
    $inner = static function (int $value) use (&$trace): int {
        $trace[] = "inner:$value";
        return $value + 1;
    };
    $outer = static function (int $value) use (&$trace): int {
        $trace[] = "outer:$value";
        return $value * 2;
    };

    $result = $outer($inner(yield 'ready'));
    yield $result;
    return $result + 1;
}

function w12_abandoned_pending_call(array &$trace): Generator
{
    $call = static function (mixed $value) use (&$trace): void {
        $trace[] = "unexpected:$value";
    };
    $call(yield 'open');
}

function w12_generator_pending_call_root(): array
{
    $trace = [];
    $generator = w12_pending_calls($trace);
    $first = $generator->current();
    $second = $generator->send(4);
    $generator->next();
    $returned = $generator->getReturn();

    $abandoned = w12_abandoned_pending_call($trace);
    $open = $abandoned->current();
    unset($abandoned);
    gc_collect_cycles();

    return [$first, $second, $returned, $open, $trace];
}
PHP,
    'w12-generator-pending-call-stack.php',
    [],
    [
        'wave' => 11,
        'function' => 'w12_generator_pending_call_root',
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
accepted result=["ready",10,11,"open",["inner:4","outer:5"]] gateway=yes runs=10 vm=0 execute_ex=0 handler=0 active=0
