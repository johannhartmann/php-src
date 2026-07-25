--TEST--
Native MIR W12 executes static coalesce edge-PHIs natively
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
function w12_static_coalesce(int $value): int
{
    static $stored = null;
    $stored ??= $value;
    return $stored;
}

function w12_static_coalesce_root(): array
{
    return [
        w12_static_coalesce(41),
        w12_static_coalesce(99),
    ];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w12-static-coalesce.php',
    [],
    ['wave' => 11, 'function' => 'w12_static_coalesce_root'],
);
printf(
    "%s return=%s vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value'] ?? null),
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
);
?>
--EXPECT--
accepted return=[41,41] vm=0 execute_ex=0 handler=0
