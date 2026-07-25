--TEST--
Native MIR W12 executes assertion control flow without VM dispatch
--INI--
zend.assertions=0
assert.exception=1
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
function w12_assert_check(bool $condition): string
{
    assert($condition);
    return $condition ? 'enabled' : 'disabled';
}
PHP;

foreach ([false, true] as $condition) {
    if ($condition) {
        ini_set('zend.assertions', '1');
    }
    $result = native_mir_test_compile_execute(
        $source,
        'w12-assert-check.php',
        [$condition],
        ['wave' => 11, 'function' => 'w12_assert_check'],
    );
    printf(
        "%s status=%s result=%s vm=%d execute_ex=%d handler=%d\n",
        $condition ? 'true' : 'false',
        $result['status'],
        json_encode($result['execution']['return_value'] ?? null),
        $result['execution']['vm_handler_calls'] ?? -1,
        $result['execution']['execute_ex_calls'] ?? -1,
        $result['execution']['opline_handler_calls'] ?? -1,
    );
}
?>
--EXPECT--
false status=accepted result="disabled" vm=0 execute_ex=0 handler=0
true status=accepted result="enabled" vm=0 execute_ex=0 handler=0
