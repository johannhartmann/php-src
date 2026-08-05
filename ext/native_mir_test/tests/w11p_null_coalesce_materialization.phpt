--TEST--
Native null coalesce fallback materializes a complete canonical zval
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
function w11p_null_coalesce_value(array $input): array
{
    $value = $input['value'] ?? null;
    return [get_debug_type($value), json_encode($value), $value];
}

function w11p_null_coalesce_materialization(): array
{
    return [
        w11p_null_coalesce_value(['value' => null]),
        w11p_null_coalesce_value([]),
        w11p_null_coalesce_value(['value' => false]),
        w11p_null_coalesce_value(['value' => 7]),
    ];
}
PHP,
    'w11p-null-coalesce-materialization.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_null_coalesce_materialization',
        'repeat' => 20,
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
accepted return=[["null","null",null],["null","null",null],["bool","false",false],["int","7",7]] closure=ready active=0
