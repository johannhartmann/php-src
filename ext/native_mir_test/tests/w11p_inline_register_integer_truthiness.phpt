--TEST--
Native baseline branches directly on exact integer machine values
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
function register_integer_truthiness(int $value): int
{
    $zero = $value - $value;
    $positive = -$value;
    $score = 0;

    if ($value) {
        $score += 1;
    } else {
        $score += 10;
    }
    if ($zero) {
        $score += 100;
    } else {
        $score += 2;
    }
    if ($positive) {
        $score += 4;
    } else {
        $score += 1000;
    }

    return $score;
}
PHP,
    'w11p-inline-register-integer-truthiness.php',
    [-7],
    [
        'wave' => 11,
        'function' => 'register_integer_truthiness',
        'repeat' => 10,
    ],
);
printf(
    "%s return=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=7 vm=0 execute_ex=0 handler=0 active=0
