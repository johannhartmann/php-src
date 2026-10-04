--TEST--
Native code preserves Zend type errors and fails entry publication atomically
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$typeErrorSource = <<<'PHP'
<?php
function native_type_error($value): int
{
    return strcmp($value, 'x');
}
PHP;

try {
    native_mir_test_compile_execute(
        $typeErrorSource,
        'type-error.php',
        [[]],
        ['function' => 'native_type_error'],
    );
    echo "type-error missing\n";
} catch (TypeError $error) {
    printf(
        "type-error=%d handler=%s frame=%s\n",
        str_starts_with(
            $error->getMessage(),
            'strcmp(): Argument #1 ($string1) must be of type string, array given',
        ),
        $error->getTrace()[0]['function'] ?? '-',
        $error->getTrace()[1]['function'] ?? '-',
    );
}

$publishSource = <<<'PHP'
<?php
function native_publish_cleanup(string $value): int
{
    return strcmp($value, 'ok');
}
PHP;
$failed = native_mir_test_compile_execute(
    $publishSource,
    'publish-failure.php',
    ['ok'],
    [
        'function' => 'native_publish_cleanup',
        'fault' => 'entry_publish_failure',
    ],
);
$failureDiagnostic = end($failed['diagnostics']);
printf(
    "publish=%s phase=%s code=%s before=%d live=%d active=%d executions=%d\n",
    $failed['status'],
    $failed['phase'],
    $failureDiagnostic['code'],
    $failed['execution']['unwind_registrations_before'],
    $failed['execution']['unwind_registrations_live'],
    $failed['execution']['entry_active_calls'],
    $failed['execution']['executions'],
);

$after = native_mir_test_compile_execute(
    $publishSource,
    'publish-after.php',
    ['ok'],
    ['function' => 'native_publish_cleanup', 'repeat' => 100],
);
printf(
    "after=%s %s return=%s before=%d live=%d active=%d executions=%d vm=%d execute_ex=%d handler=%d\n",
    $after['status'],
    $after['execution']['status'],
    json_encode($after['execution']['return_value']),
    $after['execution']['unwind_registrations_before'],
    $after['execution']['unwind_registrations_live'],
    $after['execution']['entry_active_calls'],
    $after['execution']['executions'],
    $after['execution']['vm_handler_calls'],
    $after['execution']['execute_ex_calls'],
    $after['execution']['opline_handler_calls'],
);
?>
--EXPECT--
type-error=1 handler=strcmp frame=native_type_error
publish=error phase=execute code=NATIVE0006 before=0 live=1 active=0 executions=0
after=accepted returned return=0 before=0 live=1 active=0 executions=100 vm=0 execute_ex=0 handler=0
