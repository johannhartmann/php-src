--TEST--
Native static and dynamic internal-call results are released when argument cleanup throws
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
final class NativeInternalResultProbe
{
    public static int $alive = 0;

    public function __construct()
    {
        self::$alive++;
    }

    public function __destruct()
    {
        self::$alive--;
    }
}

final class NativeThrowingArrayIterator extends ArrayIterator
{
    public function __construct()
    {
        parent::__construct([new NativeInternalResultProbe()]);
    }

    public function __destruct()
    {
        throw new RuntimeException('argument cleanup');
    }
}

function native_internal_result_cleanup_exception(): array
{
    try {
        $result = iterator_to_array(new NativeThrowingArrayIterator());
    } catch (RuntimeException $exception) {
        $static = [$exception->getMessage(), NativeInternalResultProbe::$alive];
    }

    $callable = 'iterator_to_array';
    try {
        $result = $callable(new NativeThrowingArrayIterator());
    } catch (RuntimeException $exception) {
        $dynamic = [$exception->getMessage(), NativeInternalResultProbe::$alive];
    }

    return [
        $static ?? ['static missed', NativeInternalResultProbe::$alive],
        $dynamic ?? ['dynamic missed', NativeInternalResultProbe::$alive],
    ];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'internal-result-cleanup-exception.php',
    [],
    [
        'function' => 'native_internal_result_cleanup_exception',
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
accepted return=[["argument cleanup",0],["argument cleanup",0]] runs=20 vm=0 execute_ex=0 handler=0 active=0
