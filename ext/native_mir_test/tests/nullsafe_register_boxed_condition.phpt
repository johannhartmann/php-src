--TEST--
Native nullsafe branches publish register-authoritative boxed operands
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
class NativeNullsafeBox
{
    public ?NativeNullsafeValue $value = null;
}

class NativeNullsafeValue
{
    public string $text = 'native';
}

function native_nullsafe_register_boxed_condition(): array
{
    $box = new NativeNullsafeBox();
    $withoutValue = $box->value?->text;
    $box->value = new NativeNullsafeValue();
    $withValue = $box->value?->text;

    return [$withoutValue, $withValue];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'nullsafe-register-boxed-condition.php',
    [],
    [
        'function' => 'native_nullsafe_register_boxed_condition',
        'repeat' => 20,
    ],
);

printf(
    "%s return=%s runs=%d vm=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=[null,"native"] runs=20 vm=0 active=0
