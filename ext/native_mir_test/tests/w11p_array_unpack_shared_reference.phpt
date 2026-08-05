--TEST--
Native array unpack preserves shared reference aliases
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
function w11p_array_unpack_shared_reference(): array
{
    $value = 1;
    $source = [&$value];
    $unpacked = [...$source];
    $unpacked[0] = 2;

    return [$value, $source[0], $unpacked[0]];
}
PHP,
    'w11p-array-unpack-shared-reference.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_array_unpack_shared_reference',
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
accepted return=[2,2,2] closure=ready active=0
