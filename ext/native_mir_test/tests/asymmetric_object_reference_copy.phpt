--TEST--
Native by-reference fetch of an asymmetric object property returns a copy
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
final class NativeAsymmetricReferenceValue
{
    public function __construct(public int $value)
    {
    }
}

final class NativeAsymmetricReferenceOwner
{
    public private(set) NativeAsymmetricReferenceValue $value;

    public function __construct()
    {
        $this->value = new NativeAsymmetricReferenceValue(21);
    }
}

function native_asymmetric_object_reference_copy(): int
{
    $owner = new NativeAsymmetricReferenceOwner();
    $alias =& $owner->value;
    $alias = new NativeAsymmetricReferenceValue(42);
    return $owner->value->value;
}
PHP,
    'asymmetric-object-reference-copy.php',
    [],
    [
        'function' => 'native_asymmetric_object_reference_copy',
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
accepted return=21 runs=20 vm=0 active=0
