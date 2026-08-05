--TEST--
Native string coalesce preserves the taken value through its result PHI
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
function native_string_coalesce(?string $value): string
{
    $result = $value ?? '';
    return $result;
}

function native_string_coalesce_root(): array
{
    return [
        native_string_coalesce('native'),
        native_string_coalesce(null),
    ];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-string-coalesce-phi.php',
    [],
    [
        'wave' => 11,
        'function' => 'native_string_coalesce_root',
        'repeat' => 10,
    ],
);
printf(
    "%s return=%s closure=%s active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    ($result['execution']['failed_codeunits'] ?? -1) === 0
        && ($result['execution']['performance']['ready_codeunits'] ?? -1)
            === ($result['execution']['performance']['compiled_codeunits'] ?? -2)
        ? 'ready'
        : 'incomplete',
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=["native",""] closure=ready active=0
