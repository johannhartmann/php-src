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
final class W11PInternalResultProbe
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

final class W11PThrowingArrayIterator extends ArrayIterator
{
    public function __construct()
    {
        parent::__construct([new W11PInternalResultProbe()]);
    }

    public function __destruct()
    {
        throw new RuntimeException('argument cleanup');
    }
}

function w11p_internal_result_cleanup_exception(): array
{
    try {
        $result = iterator_to_array(new W11PThrowingArrayIterator());
    } catch (RuntimeException $exception) {
        $static = [$exception->getMessage(), W11PInternalResultProbe::$alive];
    }

    $callable = 'iterator_to_array';
    try {
        $result = $callable(new W11PThrowingArrayIterator());
    } catch (RuntimeException $exception) {
        $dynamic = [$exception->getMessage(), W11PInternalResultProbe::$alive];
    }

    return [
        $static ?? ['static missed', W11PInternalResultProbe::$alive],
        $dynamic ?? ['dynamic missed', W11PInternalResultProbe::$alive],
    ];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-internal-result-cleanup-exception.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_internal_result_cleanup_exception',
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
