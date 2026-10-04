--TEST--
Native increment preserves overloaded object and ArrayAccess semantics
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
final class NativeOverloadedIncrementObject
{
    private array $values = ['value' => 1];

    public function __get(string $name): mixed
    {
        return $this->values[$name];
    }

    public function __set(string $name, mixed $value): void
    {
        $this->values[$name] = $value;
    }
}

final class NativeOverloadedIncrementArray implements ArrayAccess
{
    public function offsetExists(mixed $offset): bool
    {
        return false;
    }

    public function offsetGet(mixed $offset): mixed
    {
        return null;
    }

    public function offsetSet(mixed $offset, mixed $value): void
    {
    }

    public function offsetUnset(mixed $offset): void
    {
    }
}

function native_overloaded_incdec_semantics(): array
{
    $notices = 0;
    set_error_handler(static function () use (&$notices): bool {
        $notices++;
        return true;
    });

    $object = new NativeOverloadedIncrementObject();
    ++$object->value;

    $array = new NativeOverloadedIncrementArray();
    ++$array['value'];

    restore_error_handler();
    return [$object->value, $notices];
}
PHP,
    'overloaded-incdec-semantics.php',
    [],
    [
        'function' => 'native_overloaded_incdec_semantics',
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
accepted return=[2,1] runs=20 vm=0 active=0
