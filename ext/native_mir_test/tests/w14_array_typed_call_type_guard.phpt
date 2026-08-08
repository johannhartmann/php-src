--TEST--
Native array typed-call arguments guard their runtime types
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
function w14_array_guard_leaf(array $values): array
{
    return $values;
}

function w14_array_guard_root(array $box, string $key): array
{
    return w14_array_guard_leaf($box[$key]);
}
PHP;

$valid = native_mir_test_compile_execute(
    $source,
    'w14-array-typed-call-guard-valid.php',
    [['values' => [3, 5]], 'values'],
    [
        'wave' => 11,
        'function' => 'w14_array_guard_root',
    ],
);
printf(
    "valid=%s typed=%d helpers=%d return=%s\n",
    $valid['status'],
    $valid['execution']['performance']['direct_typed_body_sites'],
    $valid['execution']['performance']['inner_call_runtime_helper_calls'],
    json_encode($valid['execution']['return_value']),
);

try {
    native_mir_test_compile_execute(
        $source,
        'w14-array-typed-call-guard-invalid.php',
        [['values' => str_repeat('wrong', 1)], 'values'],
        [
            'wave' => 11,
            'function' => 'w14_array_guard_root',
        ],
    );
    echo "invalid=missing-error\n";
} catch (TypeError) {
    echo "invalid=TypeError\n";
}
?>
--EXPECT--
valid=accepted typed=1 helpers=0 return=[3,5]
invalid=TypeError
