--TEST--
Native baseline preserves result-producing short-circuit branches
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
function short_circuit_values(bool $left, bool $right): array
{
    $state = 0;
    $and = $left && ($state = 1);
    $afterAnd = $state;
    $or = $right || ($state = 2);
    return [$and, $afterAnd, $or, $state];
}
PHP;

foreach ([[false, true], [true, false]] as $arguments) {
    $result = native_mir_test_compile_execute(
        $source,
        'w11p-short-circuit-result-materialization.php',
        $arguments,
        [
            'wave' => 11,
            'function' => 'short_circuit_values',
            'repeat' => 10,
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
}
?>
--EXPECT--
accepted return=[false,0,true,0] vm=0 execute_ex=0 handler=0 active=0
accepted return=[true,1,true,2] vm=0 execute_ex=0 handler=0 active=0
