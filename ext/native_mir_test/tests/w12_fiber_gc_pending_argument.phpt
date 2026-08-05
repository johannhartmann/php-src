--TEST--
Native Fiber GC sees a pending outer call during nested argument evaluation
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
class W12FiberGcPendingObject
{
    private array $trace;

    public function __construct(array &$trace)
    {
        $this->trace =& $trace;
    }

    public function __destruct()
    {
        $this->trace[] = 'destroy';
    }
}

function w12_fiber_gc_pending_sink(mixed $self, mixed $value): void
{
}

function w12_fiber_gc_pending_suspend(): void
{
    Fiber::suspend('ready');
}

function w12_fiber_gc_pending_argument_root(): array
{
    $trace = [];
    $fiber = new Fiber(function () use (&$trace): void {
        $object = new W12FiberGcPendingObject($trace);
        w12_fiber_gc_pending_sink(
            Fiber::getCurrent(),
            w12_fiber_gc_pending_suspend(),
        );
    });

    $first = $fiber->start();
    $before = gc_collect_cycles();
    $trace[] = 'after-first';
    $fiber = null;
    $collected = gc_collect_cycles();

    return [$first, $before === 0, $collected > 0, $trace];
}
PHP,
    'w12-fiber-gc-pending-argument.php',
    [],
    [
        'wave' => 11,
        'function' => 'w12_fiber_gc_pending_argument_root',
        'repeat' => 10,
    ],
);

printf(
    "%s result=%s vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted result=["ready",true,true,["after-first","destroy"]] vm=0 execute_ex=0 handler=0 active=0
