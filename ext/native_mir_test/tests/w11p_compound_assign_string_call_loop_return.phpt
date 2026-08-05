--TEST--
Native baseline returns the canonical string after compound assignment in a call loop
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
function compound_assign_string_piece(string $value): string
{
    return $value;
}

function compound_assign_string_call_loop(array $values): string
{
    $data = '';
    foreach ($values as $value) {
        $data .= compound_assign_string_piece($value);
    }
    return $data;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-compound-assign-string-call-loop-return.php',
    [['alpha', '-', 'omega']],
    [
        'wave' => 11,
        'function' => 'compound_assign_string_call_loop',
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
accepted return="alpha-omega" closure=ready active=0
