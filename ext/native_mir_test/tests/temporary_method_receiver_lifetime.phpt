--TEST--
Native method calls release temporary object receivers after the call
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
function native_temporary_method_receiver_lifetime(): bool
{
    return (new ReflectionFunction(static function (): void {}))->isAnonymous();
}
PHP,
    'temporary-method-receiver-lifetime.php',
    [],
    [
        'function' => 'native_temporary_method_receiver_lifetime',
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
accepted return=true closure=ready active=0
