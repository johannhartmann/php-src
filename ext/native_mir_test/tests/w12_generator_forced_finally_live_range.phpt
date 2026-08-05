--TEST--
Native forced generator close releases active loop live ranges after finally
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
function w12_forced_finally_generator(
    &$weak,
    array &$trace,
): Generator {
    $tracked = new stdClass();
    $weak = WeakReference::create($tracked);
    $values = [$tracked, new stdClass()];
    unset($tracked);

    foreach ($values as $index => $value) {
        try {
            try {
                yield;
            } finally {
                $trace[] = "finally:$index";
            }
        } catch (RuntimeException) {
            $trace[] = 'catch';
            continue;
        }
    }
}

function w12_generator_forced_finally_live_range_root(): array
{
    $weak = null;
    $trace = [];
    w12_forced_finally_generator($weak, $trace)
        ->throw(new RuntimeException('close'));
    gc_collect_cycles();

    return [$trace, $weak->get() === null];
}
PHP,
    'w12-generator-forced-finally-live-range.php',
    [],
    [
        'wave' => 11,
        'function' => 'w12_generator_forced_finally_live_range_root',
        'repeat' => 10,
        'stack_probe' => true,
    ],
);

printf(
    "%s result=%s runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
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
accepted result=[["finally:0","catch","finally:1"],true] runs=10 vm=0 execute_ex=0 handler=0 active=0
