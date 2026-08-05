--TEST--
Native assertion calls route exceptions to the surrounding catch
--INI--
zend.assertions=1
assert.exception=1
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
function w11p_assert_exception_route(): string
{
    try {
        assert(false);
    } catch (AssertionError) {
        return 'caught';
    }

    return 'missed';
}
PHP,
    'w11p-assert-exception-route.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_assert_exception_route',
        'repeat' => 20,
    ],
);
printf(
    "%s return=%s closure=%s active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    ($result['execution']['failed_codeunits'] ?? -1) === 0
        && ($result['execution']['performance']['ready_codeunits'] ?? -1)
            === ($result['execution']['performance']['compiled_codeunits'] ?? -2)
        ? 'ready'
        : 'incomplete',
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return="caught" closure=ready active=0
