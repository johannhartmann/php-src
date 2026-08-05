--TEST--
Native nested partial-application arguments retain the call-fragment helper
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
function w12_nested_partial_target(object $bound, int $value): int
{
    return $value;
}

function w12_nested_partial_application_argument(): int
{
    $partial = w12_nested_partial_target(new stdClass(), ?);

    return $partial(42);
}
PHP,
    'w12-nested-partial-application-argument.php',
    [],
    [
        'wave' => 11,
        'function' => 'w12_nested_partial_application_argument',
        'repeat' => 20,
    ],
);
printf(
    "%s return=%d runs=%d vm=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=42 runs=20 vm=0 active=0
