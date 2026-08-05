--TEST--
Native generator resume uses the transferred heap frame
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
function w12_resume_frame_range($start, $end, $step = 1)
{
    for ($value = $start; $value <= $end; $value += $step) {
        yield $value;
    }
}

function w12_generator_resume_frame_root()
{
    $values = [];
    foreach (w12_resume_frame_range(10, 20, 2) as $value) {
        $values[] = $value;
        if (count($values) === 4) {
            break;
        }
    }
    return $values;
}
PHP,
    'w12-generator-resume-frame.php',
    [],
    [
        'wave' => 11,
        'function' => 'w12_generator_resume_frame_root',
        'repeat' => 10,
        'stack_probe' => true,
    ],
);

printf(
    "%s result=%s gateway=%s runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    ($result['execution']['generator_reentry_gateway_calls'] ?? 0) > 0
        ? 'yes' : 'no',
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted result=[10,12,14,16] gateway=yes runs=10 vm=0 execute_ex=0 handler=0 active=0
