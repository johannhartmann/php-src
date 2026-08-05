--TEST--
Native string offset assignment preserves mutable string ownership
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
function string_offset_assignment_lifetime_root(): array
{
    $value = "0123456789";
    $assigned = ($value[9] = "0");

    $negative = "abcdef";
    $negative[-1] = "Z";

    $grown = "a";
    $grown[3] = "x";

    return [$value, $assigned, $negative, $grown, strlen($value)];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-string-offset-assignment-lifetime.php',
    [],
    [
        'wave' => 11,
        'function' => 'string_offset_assignment_lifetime_root',
        'repeat' => 20,
    ],
);
printf(
    "%s return=%s runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=["0123456780","0","abcdeZ","a  x",10] runs=20 vm=0 execute_ex=0 handler=0 active=0
