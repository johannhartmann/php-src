--TEST--
Native frameless internal calls consume explicit MIR operands
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
function w11p_explicit_frameless_internal(
    string $text,
    string $needle,
    int $offset,
): int {
    $clean = trim($text);
    $position = strpos($clean, $needle, $offset);
    return min($position, 100) + strlen($clean);
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-explicit-frameless-internal.php',
    ['  alpha-beta  ', 'beta', 0],
    [
        'wave' => 11,
        'function' => 'w11p_explicit_frameless_internal',
        'repeat' => 10,
    ],
);
printf(
    "%s return=%d executions=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
);
?>
--EXPECT--
accepted return=16 executions=10 vm=0 execute_ex=0 handler=0
