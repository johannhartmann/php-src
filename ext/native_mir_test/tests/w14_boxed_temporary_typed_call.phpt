--TEST--
Native boxed temporaries feed typed component call arguments without helpers
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
declare(strict_types=1);

function w14_boxed_temporary_leaf(int $value): int
{
    return $value;
}

function w14_boxed_temporary_root(int $iterations): int
{
    $value = [0];
    for ($index = 0; $index < $iterations; $index++) {
        $value[0] = w14_boxed_temporary_leaf($value[0] + 1);
    }
    return $value[0];
}

function w14_boxed_temporary_scalar_leaf(int $value): int
{
    return $value;
}

function w14_boxed_temporary_guard_root(array $values): int
{
    return w14_boxed_temporary_scalar_leaf($values[0]);
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w14-boxed-temporary-typed-call.php',
    [25],
    [
        'wave' => 11,
        'function' => 'w14_boxed_temporary_root',
        'repeat' => 20,
    ],
);
$execution = $result['execution'];
$performance = $execution['performance'];
printf(
    "%s return=%d runs=%d codeunits=%d components=%d direct=%d typed=%d helpers=%d "
    . "allocations=%d catchers=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $execution['return_value'],
    $execution['executions'],
    $execution['native_codeunits'],
    $execution['native_components'],
    $performance['direct_call_sites'],
    $performance['direct_typed_body_sites'],
    $performance['inner_call_runtime_helper_calls'],
    $performance['inner_call_heap_allocations'],
    $performance['inner_call_catcher_boundaries'],
    $execution['vm_handler_calls'],
    $execution['execute_ex_calls'],
    $execution['opline_handler_calls'],
);

$valid = native_mir_test_compile_execute(
    $source,
    'w14-boxed-temporary-typed-call-valid.php',
    [[41]],
    [
        'wave' => 11,
        'function' => 'w14_boxed_temporary_guard_root',
    ],
);
printf(
    "valid=%s return=%d helpers=%d\n",
    $valid['status'],
    $valid['execution']['return_value'],
    $valid['execution']['performance']['inner_call_runtime_helper_calls'],
);

try {
    native_mir_test_compile_execute(
        $source,
        'w14-boxed-temporary-typed-call-invalid.php',
        [['wrong']],
        [
            'wave' => 11,
            'function' => 'w14_boxed_temporary_guard_root',
        ],
    );
    echo "invalid=missing-error\n";
} catch (TypeError) {
    echo "invalid=TypeError\n";
}
?>
--EXPECT--
accepted return=25 runs=20 codeunits=2 components=1 direct=1 typed=1 helpers=0 allocations=0 catchers=0 vm=0 execute_ex=0 handler=0
valid=accepted return=41 helpers=0
invalid=TypeError
