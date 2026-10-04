--TEST--
Native write-dimension access consumes by-reference call-result containers
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
final class NativeReferenceWriteOwner
{
    public array $values = [1];

    public function &values(): array
    {
        return $this->values;
    }

    public function replace(int &$value): void
    {
        $value = 7;
    }
}

function native_return_by_reference_write_container_lifetime(): int
{
    $owner = new NativeReferenceWriteOwner();
    $owner->values()[0]++;
    $owner->replace($owner->values()[0]);
    return $owner->values[0];
}
PHP,
    'return-by-reference-write-container-lifetime.php',
    [],
    [
        'function' => 'native_return_by_reference_write_container_lifetime',
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
accepted return=7 runs=20 vm=0 active=0
