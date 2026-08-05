--TEST--
Native direct internal calls preserve by-name finish semantics
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')
        || !function_exists('utf8_decode')) {
    die('skip required functions are not available');
}
?>
--FILE--
<?php
$previous = error_reporting(E_ALL & ~E_DEPRECATED);
$result = native_mir_test_compile_execute(
    <<<'PHP'
<?php
function w11p_internal_call_by_name(): string
{
    return utf8_decode('native');
}
PHP,
    'w11p-internal-call-by-name.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_internal_call_by_name',
        'repeat' => 10,
    ],
);
error_reporting($previous);
printf(
    "%s return=%s vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return="native" vm=0 execute_ex=0 handler=0 active=0
