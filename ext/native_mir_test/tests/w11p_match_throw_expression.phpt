--TEST--
Native W11 removes the synthetic continuation after match throw expressions
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$result = native_mir_test_compile_execute(
    <<<'PHP'
<?php
function w11p_match_throw_expression(string $value): int
{
    return match ($value) {
        'ok' => 42,
        default => throw new ValueError('bad'),
    };
}
PHP,
    'w11p-match-throw-expression.php',
    ['ok'],
    [
        'wave' => 11,
        'function' => 'w11p_match_throw_expression',
        'repeat' => 1,
    ],
);
printf(
    "%s return=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=42 vm=0 execute_ex=0 handler=0 active=0
