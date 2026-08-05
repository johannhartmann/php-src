--TEST--
Native boxed copies resolve guarded mutation results across MIR order
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
class ForwardBoxedCopyBase
{
    public function identity($value)
    {
        return $value;
    }
}

class ForwardBoxedCopyChild extends ForwardBoxedCopyBase
{
    public function apply($value)
    {
        $value++;
        if ($value >= 5) {
            return 5;
        }
        return call_user_func_array(
            [ForwardBoxedCopyBase::class, 'identity'],
            [$value],
        );
    }
}

function forward_boxed_copy_root(): array
{
    $instance = new ForwardBoxedCopyChild();
    return [$instance->apply(1), $instance->apply(4)];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-forward-boxed-copy-after-guard.php',
    [],
    [
        'wave' => 11,
        'function' => 'forward_boxed_copy_root',
        'repeat' => 10,
    ],
);
printf(
    "%s return=%s runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=[2,5] runs=10 vm=0 execute_ex=0 handler=0 active=0
