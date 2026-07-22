--TEST--
Native MIR W08 preserves Zend type errors and rejects unsupported calls atomically
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
        'w08-type-error.php',
        [[]],
        ['wave' => 8, 'function' => 'native_type_error'],
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

$unsupported = [
    'dynamic' => [
        <<<'PHP'
<?php
function unsupported_dynamic($function): int
{
    return $function('a', 'a');
}
PHP,
        'unsupported_dynamic',
        ['strcmp'],
        'MIRL0026',
    ],
    'unpack' => [
        <<<'PHP'
<?php
function unsupported_unpack(array $arguments): int
{
    return strcmp(...$arguments);
}
PHP,
        'unsupported_unpack',
        [['a', 'a']],
        'MIRL0024',
    ],
];

foreach ($unsupported as $name => [$source, $function, $arguments, $code]) {
    $result = native_mir_test_compile_execute(
        $source,
        "w08-$name.php",
        $arguments,
        ['wave' => 8, 'function' => $function],
    );
    $diagnostic = end($result['diagnostics']);
    printf(
        "%s=%s phase=%s code=%s machine=%d executions=%d vm=%d execute_ex=%d handler=%d\n",
        $name,
        $result['status'],
        $result['phase'],
        $diagnostic['code'],
        $result['execution']['machine_code'] !== null,
        $result['execution']['executions'],
        $result['execution']['vm_handler_calls'],
        $result['execution']['execute_ex_calls'],
        $result['execution']['opline_handler_calls'],
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
    'w08-publish-failure.php',
    ['ok'],
    [
        'wave' => 8,
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
    'w08-publish-after.php',
    ['ok'],
    ['wave' => 8, 'function' => 'native_publish_cleanup', 'repeat' => 100],
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
dynamic=rejected phase=lowering code=MIRL0026 machine=0 executions=0 vm=0 execute_ex=0 handler=0
unpack=rejected phase=lowering code=MIRL0024 machine=0 executions=0 vm=0 execute_ex=0 handler=0
publish=error phase=publish code=NATIVE0006 before=0 live=1 active=0 executions=0
after=accepted returned return=0 before=0 live=1 active=0 executions=100 vm=0 execute_ex=0 handler=0
