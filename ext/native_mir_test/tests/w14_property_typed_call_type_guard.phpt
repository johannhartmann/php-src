--TEST--
Native property typed-call arguments guard their runtime types
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
function w14_property_guard_leaf(string $text, array $values): array
{
    return $values;
}

function w14_property_guard_root(object $box): array
{
    return w14_property_guard_leaf($box->text, $box->values);
}
PHP;

$valid = native_mir_test_compile_execute(
    $source,
    'w14-property-typed-call-guard-valid.php',
    [(object) ['text' => str_repeat('ok', 2), 'values' => [3, 5]]],
    [
        'wave' => 11,
        'function' => 'w14_property_guard_root',
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
        'w14-property-typed-call-guard-invalid.php',
        [(object) [
            'text' => str_repeat('ok', 2),
            'values' => str_repeat('wrong', 1),
        ]],
        [
            'wave' => 11,
            'function' => 'w14_property_guard_root',
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
