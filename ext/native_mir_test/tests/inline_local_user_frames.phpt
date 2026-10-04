--TEST--
Native baseline initializes and releases local CVs in generated user frames
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
function native_inline_local_leaf($value)
{
    $copy = $value;
    $answer = 40;
    $answer += 2;
    return $answer;
}

function native_inline_local_root()
{
    $payload = 'shared payload';
    return native_inline_local_leaf($payload);
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'inline-local-user-frames.php',
    [],
    [
        'function' => 'native_inline_local_root',
        'repeat' => 10,
    ],
);
printf(
    "%s return=%d runs=%d codeunits=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['native_codeunits'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=42 runs=10 codeunits=2 vm=0 execute_ex=0 handler=0 active=0
