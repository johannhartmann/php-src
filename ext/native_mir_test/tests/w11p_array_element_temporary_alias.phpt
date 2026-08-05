--TEST--
Native temporary scalar array elements survive reused frame slots
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
function array_element_temporary_alias(): array
{
    $source = 'abc';
    $first = strlen($source);
    $empty = '';
    return [$first, strlen($empty)];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-array-element-temporary-alias.php',
    [],
    [
        'wave' => 11,
        'function' => 'array_element_temporary_alias',
        'repeat' => 20,
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
accepted return=[3,0] vm=0 execute_ex=0 handler=0 active=0
