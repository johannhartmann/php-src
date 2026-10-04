--TEST--
Native by-reference arguments do not attach type sources to untyped properties
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
final class NativeUntypedPropertyOwner
{
    public $values = [21];
}

final class NativeUntypedPropertyAlias
{
    private NativeUntypedPropertyOwner $owner;
    private array $values;

    public function __construct(
        NativeUntypedPropertyOwner $owner,
        array &$values,
    ) {
        $this->owner = $owner;
        $this->values =& $values;
    }

    public function update(): int
    {
        $this->values[0] *= 2;
        return $this->owner->values[0];
    }
}

function native_untyped_property_byref_lifetime(): int
{
    $owner = new NativeUntypedPropertyOwner();
    $alias = new NativeUntypedPropertyAlias($owner, $owner->values);
    return $alias->update();
}
PHP,
    'untyped-property-byref-lifetime.php',
    [],
    [
        'function' => 'native_untyped_property_byref_lifetime',
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
