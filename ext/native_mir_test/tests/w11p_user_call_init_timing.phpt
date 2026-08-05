--TEST--
Native user-call target errors occur before argument side effects
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
function w11p_init_side_effect(int &$state): int
{
    $state = 99;
    return $state;
}

function w11p_user_call_init_timing(): int
{
    $state = 42;
    try {
        w11p_missing_target(w11p_init_side_effect($state));
    } catch (Error $error) {
    }
    return $state;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-user-call-init-timing.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_user_call_init_timing',
        'repeat' => 10,
    ],
);
$execution = $result['execution'];
printf(
    "%s return=%d runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $execution['return_value'],
    $execution['executions'],
    $execution['vm_handler_calls'],
    $execution['execute_ex_calls'],
    $execution['opline_handler_calls'],
    $execution['entry_active_calls'],
);
?>
--EXPECT--
accepted return=42 runs=10 vm=0 execute_ex=0 handler=0 active=0
