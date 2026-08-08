--TEST--
Native conditional call assignment preserves the loop PHI on both branches
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
function conditional_call_loop_leaf(): int
{
    return 10;
}

function conditional_call_loop(array $values, bool $take): int
{
    $count = 0;
    foreach ($values as $value) {
        $count++;
        if ($take) {
            $count += conditional_call_loop_leaf();
        }
    }
    return $count;
}

function conditional_call_loop_root(): array
{
    return [
        conditional_call_loop([1], false),
        conditional_call_loop([1, 2], false),
        conditional_call_loop([1], true),
    ];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w14-conditional-call-loop-phi.php',
    [],
    [
        'wave' => 11,
        'function' => 'conditional_call_loop_root',
    ],
);
printf(
    "%s return=%s vm=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['vm_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=[1,2,11] vm=0 active=0
