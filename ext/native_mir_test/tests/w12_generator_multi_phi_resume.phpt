--TEST--
Native generator resume preserves multiple loop-carried PHIs
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
function w12_generator_multi_phi_values(): Generator
{
    $offset = 0;
    yield true;
    for ($index = 0; $index < 100; $index++) {
        $offset++;
        yield true;
    }
    return $offset;
}

function w12_generator_multi_phi_resume_root(): int
{
    $generator = w12_generator_multi_phi_values();
    foreach ($generator as $value) {
    }
    return $generator->getReturn();
}
PHP,
    'w12-generator-multi-phi-resume.php',
    [],
    [
        'wave' => 11,
        'function' => 'w12_generator_multi_phi_resume_root',
        'repeat' => 10,
        'stack_probe' => true,
    ],
);

printf(
    "%s result=%d gateway=%s runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
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
accepted result=100 gateway=yes runs=10 vm=0 execute_ex=0 handler=0 active=0
