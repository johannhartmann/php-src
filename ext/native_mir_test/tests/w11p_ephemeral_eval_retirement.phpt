--TEST--
Native MIR retires completed eval codeunits without declarations or closures
--INI--
memory_limit=32M
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
function w11p_ephemeral_eval_retirement(): array {
    $before = memory_get_usage();
    $sum = 0;
    for ($index = 0; $index < 2048; $index++) {
        $sum += eval('return 1;');
    }
    return [$sum, memory_get_usage() - $before < 1024 * 1024];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-ephemeral-eval-retirement.php',
    [],
    ['wave' => 11, 'function' => 'w11p_ephemeral_eval_retirement'],
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
accepted return=[2048,true] vm=0 execute_ex=0 handler=0
