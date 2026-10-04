--TEST--
Native baseline places extra user arguments and releases aliased locals correctly
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
final class NativeFrameCleanupProbe
{
    public static int $destroyed = 0;

    public function __destruct()
    {
        self::$destroyed++;
    }
}

function native_extra_leaf(int $base): int
{
    $arguments = func_get_args();
    $tail = array_slice(func_get_args(), 1);
    return $base
        + strlen($arguments[1])
        + $tail[1][0]
        + func_num_args();
}

function native_alias_cleanup_leaf(): int
{
    $object = new NativeFrameCleanupProbe();
    $alias = $object;
    return 5;
}

function native_extra_root(): int
{
    $text = 'abc';
    $values = [7];
    $extra = native_extra_leaf(10, $text, $values);
    $cleanup = native_alias_cleanup_leaf();
    return $extra * 10 + $cleanup + NativeFrameCleanupProbe::$destroyed;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'inline-extra-user-frames.php',
    [],
    [
        'function' => 'native_extra_root',
        'repeat' => 10,
    ],
);
if ($result['status'] !== 'accepted') {
    printf(
        "%s diagnostics=%s\n",
        $result['status'],
        json_encode($result['diagnostics'] ?? null),
    );
    return;
}
printf(
    "%s return=%d runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=245 runs=10 vm=0 execute_ex=0 handler=0 active=0
