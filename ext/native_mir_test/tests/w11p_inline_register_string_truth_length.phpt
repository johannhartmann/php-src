--TEST--
Native baseline keeps local string truthiness and length register-authoritative
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
function inline_register_string_truth_length(int $count): int
{
    $empty = '';
    $zero = '0';
    $doubleZero = '00';
    $native = 'native';
    $score = 0;

    for ($index = 0; $index < $count; $index++) {
        if ($empty) {
            $score += 10000;
        }
        if ($zero) {
            $score += 1000;
        }
        if ($doubleZero) {
            $score += 100;
        }
        if ($native) {
            $score += 10;
        }
        $score += strlen($empty);
        $score += strlen($zero);
        $score += strlen($doubleZero);
        $score += strlen($native);
    }

    return $score;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-inline-register-string-truth-length.php',
    [5],
    [
        'wave' => 11,
        'function' => 'inline_register_string_truth_length',
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
accepted return=595 vm=0 execute_ex=0 handler=0 active=0
