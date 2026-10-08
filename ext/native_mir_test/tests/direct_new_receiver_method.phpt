--TEST--
Native baseline resolves an exact method receiver through a NEW SSA chain
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
final class NativeConstructedReceiver
{
    public function step(int $value): int
    {
        return $value + 1;
    }
}

function native_constructed_receiver(int $count): int
{
    $receiver = new NativeConstructedReceiver();
    $value = 0;
    for ($index = 0; $index < $count; $index++) {
        $value = $receiver->step($value);
    }
    return $value;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'direct-new-receiver-method.php',
    [1000],
    [
        'function' => 'native_constructed_receiver',
        'repeat' => 10,
    ],
);
$performance = $result['execution']['performance'];
printf(
    "%s return=%d runs=%d direct=%d inner_helpers=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $performance['direct_call_sites'],
    $performance['inner_call_runtime_helper_calls'],
);
?>
--EXPECT--
accepted return=1000 runs=10 direct=1 inner_helpers=0
