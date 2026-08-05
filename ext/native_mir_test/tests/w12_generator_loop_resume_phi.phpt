--TEST--
Native generator resume preserves a copied loop PHI before assign-op
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
function w12_generator_range($start, $end, $step = 1)
{
    for ($value = $start; $value <= $end; $value += $step) {
        yield $value;
    }
}

function w12_generator_resume_compare($levels)
{
    foreach (range(0, 2 << $levels) as $value) {
        yield $value;
        if ($value === 14) {
            throw new RuntimeException('resume comparison');
        }
    }
}

function w12_generator_loop_resume_phi_root()
{
    $generator = w12_generator_range(10, 20, 2);
    $values = [];
    foreach ($generator as $value) {
        $values[] = $value;
    }

    $compared = [];
    $caught = false;
    try {
        foreach (w12_generator_resume_compare(5) as $value) {
            $compared[] = $value;
        }
    } catch (RuntimeException $exception) {
        $caught = $exception->getMessage() === 'resume comparison';
    }

    return [$values, count($compared), end($compared), $caught];
}
PHP,
    'w12-generator-loop-resume-phi.php',
    [],
    [
        'wave' => 11,
        'function' => 'w12_generator_loop_resume_phi_root',
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
accepted result=[[10,12,14,16,18,20],15,14,true] gateway=yes runs=10 vm=0 execute_ex=0 handler=0 active=0
