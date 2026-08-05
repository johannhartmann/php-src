--TEST--
Native source scalar results feed direct internal call arguments
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
function source_scalar_internal_argument(string $value): int
{
    var_dump(strlen($value));
    return strlen($value);
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-source-scalar-internal-argument.php',
    ['abc'],
    [
        'wave' => 11,
        'function' => 'source_scalar_internal_argument',
    ],
);
printf(
    "%s return=%d vm=%d execute_ex=%d handlers=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
);
?>
--EXPECT--
int(3)
accepted return=3 vm=0 execute_ex=0 handlers=0
