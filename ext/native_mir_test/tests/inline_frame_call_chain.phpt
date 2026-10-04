--TEST--
Native inline-frame call results feed subsequent call arguments
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
function native_chain_times3(int $value): int
{
    return $value * 3;
}

function native_chain_times5(int $value): int
{
    return $value * 5;
}

class NativeInlineFrameMultiplier
{
    public function times7(int $value): int
    {
        return $value * 7;
    }
}

function native_inline_frame_call_chain(): int
{
    $multiplier = new NativeInlineFrameMultiplier();
    return 1
        |> native_chain_times3(...)
        |> 'native_chain_times5'
        |> $multiplier->times7(...);
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'inline-frame-call-chain.php',
    [],
    [
        'function' => 'native_inline_frame_call_chain',
        'repeat' => 20,
    ],
);
$execution = $result['execution'];
$performance = $execution['performance'];
printf(
    "%s return=%d runs=%d active=%d vm=%d execute_ex=%d handlers=%d calls=%d frame=%s\n",
    $result['status'],
    $execution['return_value'],
    $execution['executions'],
    $execution['entry_active_calls'],
    $execution['vm_handler_calls'],
    $execution['execute_ex_calls'],
    $execution['opline_handler_calls'],
    $performance['direct_call_sites'],
    $performance['direct_call_frame_bytes'] > 0 ? 'yes' : 'no',
);
?>
--EXPECT--
accepted return=105 runs=20 active=0 vm=0 execute_ex=0 handlers=0 calls=3 frame=yes
