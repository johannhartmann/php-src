--TEST--
Native MIR W12 discards delayed exceptions and incomplete returns natively
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
function w12_exception_cleanup(): int
{
    try {
        throw new RuntimeException('discarded');
    } catch (RuntimeException) {
        return 1;
    } finally {
        return 2;
    }
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w12-exception-cleanup.php',
    [],
    ['wave' => 11, 'function' => 'w12_exception_cleanup'],
);
printf(
    "%s result=%s vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value'] ?? null),
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
);
?>
--EXPECT--
accepted result=2 vm=0 execute_ex=0 handler=0
