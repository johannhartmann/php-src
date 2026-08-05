--TEST--
Native direct internal calls preserve deprecation and no-discard diagnostics
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')
        || !function_exists('utf8_decode')
        || !function_exists('zend_test_deprecated_nodiscard')) {
    die('skip required functions are not available');
}
?>
--FILE--
<?php
$result = native_mir_test_compile_execute(
    <<<'PHP'
<?php
function w11p_internal_call_metadata(): void
{
    utf8_decode('native');
    zend_test_deprecated_nodiscard();
}
PHP,
    'w11p-internal-call-metadata.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_internal_call_metadata',
        'repeat' => 1,
    ],
);
printf(
    "%s vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECTF--
Deprecated: Function utf8_decode() is deprecated since 8.2, visit the php.net documentation for various alternatives in w11p-internal-call-metadata.php on line 4

Deprecated: Function zend_test_deprecated_nodiscard() is deprecated, custom message in w11p-internal-call-metadata.php on line 5

Warning: The return value of function zend_test_deprecated_nodiscard() should either be used or intentionally ignored by casting it as (void), custom message 2 in w11p-internal-call-metadata.php on line 5
accepted vm=0 execute_ex=0 handler=0 active=0
