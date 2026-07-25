--TEST--
Native baseline reads string length directly from boxed CV storage
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
function inline_string_length($value)
{
    $length = strlen($value);
    return [$value, $length];
}

function inline_string_length_leaf(string $value): int
{
    return strlen($value);
}

function inline_string_length_root(string $value): int
{
    return inline_string_length_leaf($value);
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-inline-string-length.php',
    ['native-string'],
    [
        'wave' => 11,
        'function' => 'inline_string_length',
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

$direct = native_mir_test_compile_execute(
    $source,
    'w11p-inline-string-length-direct.php',
    ['native-string'],
    [
        'wave' => 11,
        'function' => 'inline_string_length_root',
        'repeat' => 10,
    ],
);
printf(
    "%s direct=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $direct['status'],
    $direct['execution']['return_value'],
    $direct['execution']['vm_handler_calls'],
    $direct['execution']['execute_ex_calls'],
    $direct['execution']['opline_handler_calls'],
    $direct['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=["native-string",13] vm=0 execute_ex=0 handler=0 active=0
accepted direct=13 vm=0 execute_ex=0 handler=0 active=0
