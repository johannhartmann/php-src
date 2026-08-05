--TEST--
Native baseline carries compound-assignment call results through loop PHIs
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
function compound_assign_call_length(string $value): int
{
    return strlen($value);
}

function compound_assign_call_loop(array $values): int
{
    $length = 0;
    foreach ($values as $value) {
        $length += compound_assign_call_length($value);
    }
    return $length;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-compound-assign-call-loop-phi.php',
    [['1234567', 'abcdefg', 'ABCDEFG']],
    [
        'wave' => 11,
        'function' => 'compound_assign_call_loop',
        'repeat' => 10,
    ],
);
printf(
    "%s return=%d closure=%s active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    ($result['execution']['failed_codeunits'] ?? -1) === 0
        && ($result['execution']['performance']['ready_codeunits'] ?? -1)
            === ($result['execution']['performance']['compiled_codeunits'] ?? -2)
        ? 'ready'
        : 'incomplete',
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=21 closure=ready active=0
