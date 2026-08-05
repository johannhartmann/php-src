--TEST--
Native return reloads a string mutated through an offset assignment
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
function string_offset_return_authority_root(): string
{
    $value = 'bpache';
    $value[0] = 'a';
    return $value;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-string-offset-return-authority.php',
    [],
    [
        'wave' => 11,
        'function' => 'string_offset_return_authority_root',
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
accepted return="apache" runs=20 vm=0 execute_ex=0 handler=0 active=0
