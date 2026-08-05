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
final class W11PAsymmetricReferenceValue
{
    public function __construct(public int $value)
    {
    }
}

final class W11PAsymmetricReferenceOwner
{
    public private(set) W11PAsymmetricReferenceValue $value;

    public function __construct()
    {
        $this->value = new W11PAsymmetricReferenceValue(21);
    }
}

function w11p_asymmetric_object_reference_copy(): int
{
    $owner = new W11PAsymmetricReferenceOwner();
    $alias =& $owner->value;
    $alias = new W11PAsymmetricReferenceValue(42);
    return $owner->value->value;
}
PHP,
    'w11p-asymmetric-object-reference-copy.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_asymmetric_object_reference_copy',
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
