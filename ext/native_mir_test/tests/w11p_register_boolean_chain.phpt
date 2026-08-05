--TEST--
Native baseline keeps exact isset boolean chains in registers
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
function register_boolean_chain(int $iterations): int
{
    $sum = 0;
    $value = 1;
    for ($index = 0; $index < $iterations; $index++) {
        if (isset($value) && !empty($value)) {
            $sum++;
        }
    }
    return $sum;
}
PHP,
    'w11p-register-boolean-chain.php',
    [50],
    [
        'wave' => 11,
        'function' => 'register_boolean_chain',
        'repeat' => 10,
    ],
);
$performance = $result['execution']['performance'];
printf(
    "%s return=%d guards=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $performance['guard_sites'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
$edges = native_mir_test_compile_execute(
    <<<'PHP'
<?php
function register_boolean_edges(int $iterations): int
{
    $sum = 0;
    for ($index = 0; $index < $iterations; $index++) {
        $value = $index & 1;
        if (isset($value) && !empty($value)) {
            $sum++;
        }
        if (!isset($value) || empty($value)) {
            $sum += 2;
        }
    }
    return $sum;
}
PHP,
    'w11p-register-boolean-edges.php',
    [50],
    [
        'wave' => 11,
        'function' => 'register_boolean_edges',
        'repeat' => 10,
    ],
);
printf(
    "%s return=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $edges['status'],
    $edges['execution']['return_value'],
    $edges['execution']['vm_handler_calls'],
    $edges['execution']['execute_ex_calls'],
    $edges['execution']['opline_handler_calls'],
    $edges['execution']['entry_active_calls'],
);
$terminal = native_mir_test_compile_execute(
    <<<'PHP'
<?php
function register_boolean_terminal(int $iterations): int
{
    $sum = 0;
    $value = 1;
    for ($index = 0; $index < $iterations; $index++) {
        if (!empty($value)) {
            $sum++;
        }
    }
    return $sum;
}
PHP,
    'w11p-register-boolean-terminal.php',
    [50],
    [
        'wave' => 11,
        'function' => 'register_boolean_terminal',
        'repeat' => 10,
    ],
);
printf(
    "%s return=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $terminal['status'],
    $terminal['execution']['return_value'],
    $terminal['execution']['vm_handler_calls'],
    $terminal['execution']['execute_ex_calls'],
    $terminal['execution']['opline_handler_calls'],
    $terminal['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=50 guards=3 vm=0 execute_ex=0 handler=0 active=0
accepted return=75 vm=0 execute_ex=0 handler=0 active=0
accepted return=50 vm=0 execute_ex=0 handler=0 active=0
