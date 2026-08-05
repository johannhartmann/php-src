--TEST--
Native named user calls expand a by-reference argument before scalar tail arguments
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
function w11p_named_reference_root(): int
{
    if (time() > 0) {
        function w11p_named_reference_target(&$value, $next): int
        {
            $value = $next;
            return $value;
        }
    }

    $value = null;
    return w11p_named_reference_target($value, 21) + $value;
}
PHP,
    'w11p-named-by-reference-user-call.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_named_reference_root',
    ],
);
printf(
    "%s return=%d codeunits=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['native_codeunits'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=42 codeunits=2 vm=0 execute_ex=0 handler=0 active=0
