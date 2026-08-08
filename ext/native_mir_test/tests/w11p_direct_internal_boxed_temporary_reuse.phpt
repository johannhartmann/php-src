--TEST--
Native direct internal calls preserve boxed arguments from reused temporary slots
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$first = -0.000516528926;
$second = 548.0;
$values = [
    'first' => &$first,
    'second' => &$second,
];

$source = <<<'PHP'
<?php
function direct_internal_boxed_temporary_reuse(array $values): string
{
    return sprintf(
        "%.12f;%.12f",
        $values['first'],
        $values['second'],
    );
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-direct-internal-boxed-temporary-reuse.php',
    [$values],
    [
        'wave' => 11,
        'function' => 'direct_internal_boxed_temporary_reuse',
        'repeat' => 10,
    ],
);
printf(
    "%s %s return=%s runs=%d active=%d\n",
    $result['status'],
    $result['execution']['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted returned return=-0.000516528926;548.000000000000 runs=10 active=0
