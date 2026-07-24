--TEST--
Native baseline materializes optional user-call defaults in generated frames
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
function w11p_optional_leaf(
    int $left,
    int $right = 2,
    string $label = 'native',
    array $values = [3, 4],
): int {
    return $left + $right + strlen($label) + $values[1];
}

function w11p_optional_root(): int
{
    return w11p_optional_leaf(1)
        + w11p_optional_leaf(1, 5, 'abc', [6, 7]);
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-inline-optional-user-frames.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_optional_root',
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
accepted return=29 runs=10 codeunits=2 vm=0 execute_ex=0 handler=0 active=0
