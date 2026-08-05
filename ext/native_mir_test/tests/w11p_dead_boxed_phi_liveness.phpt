--TEST--
Native compilation ignores boxed PHIs without frozen machine uses
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
function w11p_dead_boxed_phi_liveness(): string
{
    try {
        constant('\NonExistantClass::non_existant_constant');
    } catch (Throwable|Error|Exception $exception) {
        return $exception->getMessage();
    }
    return 'unexpected';
}
PHP,
    'w11p-dead-boxed-phi-liveness.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_dead_boxed_phi_liveness',
    ],
);
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
accepted return="Class \"NonExistantClass\" not found" vm=0 execute_ex=0 handler=0 active=0
