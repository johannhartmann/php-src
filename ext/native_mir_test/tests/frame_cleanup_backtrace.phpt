--TEST--
Native frame cleanup hides destructed arguments from reentrant backtraces
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
final class NativeFrameCleanupBacktrace
{
    public string $value;

    public function __destruct()
    {
        unset($this->value);
        foreach ((new Exception())->getTrace() as $frame) {
            if (($frame['function'] ?? null)
                    === 'native_frame_cleanup_backtrace') {
                throw new Error('Dying native frame remained visible');
            }
        }
    }
}

function native_frame_cleanup_backtrace(string $argument): int
{
    $argument = str_shuffle(str_repeat('A', 79));
    $object = new NativeFrameCleanupBacktrace();
    $object->value = $argument;
    return 42;
}
PHP,
    'frame-cleanup-backtrace.php',
    ['x'],
    [
        'function' => 'native_frame_cleanup_backtrace',
        'repeat' => 20,
    ],
);

printf(
    "%s return=%d runs=%d vm=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=42 runs=20 vm=0 active=0
