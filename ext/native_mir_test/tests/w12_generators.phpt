--TEST--
Native generator frames suspend, resume, delegate and return without VM dispatch
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$cases = [
    'state' => <<<'PHP'
<?php
function w12_state_generator(int $value): Generator
{
    $local = $value + 1;
    yield $local;
    $local += 2;
    yield $local;
    return $local + 3;
}
function w12_state_root(): array
{
    $generator = w12_state_generator(4);
    $first = $generator->current();
    $generator->next();
    $second = $generator->current();
    $generator->next();
    return [$first, $second, $generator->getReturn()];
}
PHP,
    'send-and-keys' => <<<'PHP'
<?php
function w12_send_generator(): Generator
{
    $sent = yield 5 => 11;
    yield $sent;
    return $sent + 1;
}
function w12_send_root(): array
{
    $generator = w12_send_generator();
    $first = [$generator->key(), $generator->current()];
    $second = $generator->send(7);
    $secondKey = $generator->key();
    $generator->next();
    return [$first, [$secondKey, $second], $generator->getReturn()];
}
PHP,
    'delegation' => <<<'PHP'
<?php
function w12_inner_generator(): Generator
{
    yield 4;
    return 6;
}
function w12_delegate_generator(): Generator
{
    yield from [1, 2];
    $result = yield from w12_inner_generator();
    yield $result;
    return 8;
}
function w12_delegate_root(): array
{
    $generator = w12_delegate_generator();
    $values = [];
    while ($generator->valid()) {
        $values[] = $generator->current();
        $generator->next();
    }
    $values[] = $generator->getReturn();
    return $values;
}
PHP,
    'throw' => <<<'PHP'
<?php
function w12_throw_generator(): Generator
{
    try {
        yield 'ready';
    } catch (RuntimeException $exception) {
        yield 'caught:' . $exception->getMessage();
        return 17;
    }
    return -1;
}
function w12_throw_root(): array
{
    $generator = w12_throw_generator();
    $first = $generator->current();
    $second = $generator->throw(new RuntimeException('boom'));
    $generator->next();
    return [$first, $second, $generator->getReturn()];
}
PHP,
    'by-reference' => <<<'PHP'
<?php
function &w12_by_reference_generator(int &$value): Generator
{
    yield $value;
    $value += 2;
    yield $value;
    return $value;
}
function w12_by_reference_root(): array
{
    $value = 4;
    $generator = w12_by_reference_generator($value);
    $trace = [];
    foreach ($generator as &$yielded) {
        $trace[] = [$value, $yielded];
        $yielded += 10;
        $trace[] = [$value, $yielded];
    }
    unset($yielded);
    return [$value, $generator->getReturn(), $trace];
}
PHP,
    'dynamic-owners' => <<<'PHP'
<?php
class W12GeneratorOwner
{
    public function values(int $base): Generator
    {
        yield $base;
        return $base + 1;
    }
}
function w12_dynamic_owners_root(): array
{
    $method = new W12GeneratorOwner()->values(5);
    $closureFactory = static fn (int $base): Generator => (function () use ($base): Generator {
        yield $base;
        return $base + 2;
    })();
    $closure = $closureFactory(7);
    eval('function w12_eval_generator(int $base): Generator {
        yield $base;
        return $base + 3;
    }');
    $evaluated = w12_eval_generator(9);
    $values = [
        $method->current(),
        $closure->current(),
        $evaluated->current(),
    ];
    $method->next();
    $closure->next();
    $evaluated->next();
    return [
        $values,
        $method->getReturn(),
        $closure->getReturn(),
        $evaluated->getReturn(),
    ];
}
PHP,
];

foreach ($cases as $name => $source) {
    $function = 'w12_' . match ($name) {
        'state' => 'state_root',
        'send-and-keys' => 'send_root',
        'delegation' => 'delegate_root',
        'throw' => 'throw_root',
        'by-reference' => 'by_reference_root',
        'dynamic-owners' => 'dynamic_owners_root',
    };
    $result = native_mir_test_compile_execute(
        $source,
        "w12-$name.php",
        [],
        ['wave' => 11, 'function' => $function],
    );
    printf(
        "%s status=%s result=%s vm=%d execute_ex=%d handler=%d\n",
        $name,
        $result['status'],
        json_encode($result['execution']['return_value']),
        $result['execution']['vm_handler_calls'],
        $result['execution']['execute_ex_calls'],
        $result['execution']['opline_handler_calls'],
    );
}
?>
--EXPECT--
state status=accepted result=[5,7,10] vm=0 execute_ex=0 handler=0
send-and-keys status=accepted result=[[5,11],[6,7],8] vm=0 execute_ex=0 handler=0
delegation status=accepted result=[1,2,4,6,8] vm=0 execute_ex=0 handler=0
throw status=accepted result=["ready","caught:boom",17] vm=0 execute_ex=0 handler=0
by-reference status=accepted result=[26,26,[[4,4],[14,14],[16,16],[26,26]]] vm=0 execute_ex=0 handler=0
dynamic-owners status=accepted result=[[5,7,9],6,9,12] vm=0 execute_ex=0 handler=0
