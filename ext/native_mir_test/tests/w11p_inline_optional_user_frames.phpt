--TEST--
Native universal user calls own optional persistent defaults without call helpers
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
    $label .= '!';
    $values[0] = 99;
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
$performance = $result['execution']['performance'];
printf(
    "%s return=%d runs=%d codeunits=%d direct=%d helpers=%d allocations=%d "
    . "catchers=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['native_codeunits'],
    $performance['direct_call_sites'],
    $performance['inner_call_runtime_helper_calls'],
    $performance['inner_call_heap_allocations'],
    $performance['inner_call_catcher_boundaries'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=31 runs=10 codeunits=2 direct=2 helpers=0 allocations=0 catchers=0 vm=0 execute_ex=0 handler=0 active=0
