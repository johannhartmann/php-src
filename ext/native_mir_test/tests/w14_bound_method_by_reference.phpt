--TEST--
Native bound method calls honor concrete by-reference parameter declarations
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
final class W14BoundReferenceTarget
{
    public function replace(array &$left, array &$right): int
    {
        $left[0] = 41;
        $right[0] = 42;
        return $left[0] + $right[0];
    }
}

function w14_bound_method_by_reference_root(): array
{
    $left = [1];
    $right = [2];
    $target = new W14BoundReferenceTarget();
    $sum = $target->replace($left, $right);
    return [$sum, $left[0], $right[0]];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w14-bound-method-by-reference.php',
    [],
    [
        'wave' => 11,
        'function' => 'w14_bound_method_by_reference_root',
        'repeat' => 20,
    ],
);
$execution = $result['execution'];
printf(
    "%s return=%s runs=%d codeunits=%d components=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    json_encode($execution['return_value']),
    $execution['executions'],
    $execution['native_codeunits'],
    $execution['native_components'],
    $execution['vm_handler_calls'],
    $execution['execute_ex_calls'],
    $execution['opline_handler_calls'],
);
?>
--EXPECT--
accepted return=[83,41,42] runs=20 codeunits=2 components=1 vm=0 execute_ex=0 handler=0
