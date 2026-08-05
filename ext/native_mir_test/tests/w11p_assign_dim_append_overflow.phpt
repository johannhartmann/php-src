--TEST--
Native dimension assignments report append index overflow as PHP errors
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
function w11p_assign_dim_append_overflow(): array
{
    $messages = [];

    $values = [PHP_INT_MAX => 42];
    try {
        $values[] = 123;
    } catch (Error $exception) {
        $messages[] = $exception->getMessage();
    }

    $values = [PHP_INT_MAX => 42];
    try {
        $values[] += 123;
    } catch (Error $exception) {
        $messages[] = $exception->getMessage();
    }

    return $messages;
}
PHP,
    'w11p-assign-dim-append-overflow.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_assign_dim_append_overflow',
        'repeat' => 20,
    ],
);

printf(
    "%s return=%s closure=%s vm=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    ($result['execution']['failed_codeunits'] ?? -1) === 0
        && ($result['execution']['performance']['ready_codeunits'] ?? -1)
            === ($result['execution']['performance']['compiled_codeunits'] ?? -2)
        ? 'ready'
        : 'incomplete',
    $result['execution']['vm_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=["Cannot add element to the array as the next element is already occupied","Cannot add element to the array as the next element is already occupied"] closure=ready vm=0 active=0
