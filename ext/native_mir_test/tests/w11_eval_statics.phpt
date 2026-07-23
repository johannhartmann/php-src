--TEST--
Native MIR W11 keeps repeated eval static storage distinct and releases it
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
function w11_eval_statics(): array {
    return [
        eval('static $value = 0; return ++$value;'),
        eval('static $value = 0; return ++$value;'),
    ];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11-eval-statics.php',
    [],
    ['wave' => 11, 'function' => 'w11_eval_statics'],
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
accepted return=[1,1] vm=0 execute_ex=0 handler=0
