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
final class W11PUntypedPropertyOwner
{
    public $values = [21];
}

final class W11PUntypedPropertyAlias
{
    private W11PUntypedPropertyOwner $owner;
    private array $values;

    public function __construct(
        W11PUntypedPropertyOwner $owner,
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

function w11p_untyped_property_byref_lifetime(): int
{
    $owner = new W11PUntypedPropertyOwner();
    $alias = new W11PUntypedPropertyAlias($owner, $owner->values);
    return $alias->update();
}
PHP,
    'w11p-untyped-property-byref-lifetime.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_untyped_property_byref_lifetime',
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
