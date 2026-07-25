--TEST--
Native frames survive Fiber suspend, resume, throw and nested stack switches
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$cases = [
    'resume' => <<<'PHP'
<?php
function w12_fiber_resume_root(): array
{
    $fiber = new Fiber(function (int $value): int {
        $local = $value + 1;
        $received = Fiber::suspend($local);
        return $received + 2;
    });
    $first = $fiber->start(4);
    $fiber->resume(7);
    return [$first, $fiber->getReturn()];
}
PHP,
    'throw' => <<<'PHP'
<?php
function w12_fiber_throw_root(): array
{
    $fiber = new Fiber(function (): string {
        try {
            Fiber::suspend('ready');
        } catch (RuntimeException $exception) {
            return 'caught:' . $exception->getMessage();
        }
        return 'missed';
    });
    $first = $fiber->start();
    $fiber->throw(new RuntimeException('boom'));
    return [$first, $fiber->getReturn()];
}
PHP,
    'nested' => <<<'PHP'
<?php
function w12_fiber_nested_root(): array
{
    $outer = new Fiber(function (): array {
        $inner = new Fiber(function (): int {
            $value = Fiber::suspend(3);
            return $value + 4;
        });
        $first = $inner->start();
        $inner->resume(5);
        $resume = Fiber::suspend([$first, $inner->getReturn()]);
        return [$resume, 9];
    });
    $first = $outer->start();
    $outer->resume(8);
    return [$first, $outer->getReturn()];
}
PHP,
    'callback' => <<<'PHP'
<?php
function w12_fiber_callback_leaf(int $value): int
{
    return $value * 2;
}
function w12_fiber_callback_root(): array
{
    $fiber = new Fiber(function (): int {
        $mapped = array_map('w12_fiber_callback_leaf', [2, 3]);
        $resume = Fiber::suspend($mapped);
        return w12_fiber_callback_leaf($resume);
    });
    $first = $fiber->start();
    $fiber->resume(7);
    return [$first, $fiber->getReturn()];
}
PHP,
];

foreach ($cases as $name => $source) {
    $result = native_mir_test_compile_execute(
        $source,
        "w12-fiber-$name.php",
        [],
        ['wave' => 11, 'function' => "w12_fiber_{$name}_root"],
    );
    printf(
        "%s status=%s result=%s vm=%d execute_ex=%d handler=%d active=%d\n",
        $name,
        $result['status'],
        json_encode($result['execution']['return_value']),
        $result['execution']['vm_handler_calls'],
        $result['execution']['execute_ex_calls'],
        $result['execution']['opline_handler_calls'],
        $result['execution']['entry_active_calls'],
    );
}
?>
--EXPECT--
resume status=accepted result=[5,9] vm=0 execute_ex=0 handler=0 active=0
throw status=accepted result=["ready","caught:boom"] vm=0 execute_ex=0 handler=0 active=0
nested status=accepted result=[[3,9],[8,9]] vm=0 execute_ex=0 handler=0 active=0
callback status=accepted result=[[4,6],14] vm=0 execute_ex=0 handler=0 active=0
