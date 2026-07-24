--TEST--
Native baseline executes boxed and temporary echo semantics
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$referenceSource = <<<'PHP'
<?php
function selected(&$value): int
{
    echo $value;
    return 9;
}
PHP;

$value = 'ref';
$referenceResult = native_mir_test_compile_execute(
    $referenceSource,
    'w11p-value-echo-reference.php',
    [&$value],
    ['wave' => 11, 'function' => 'selected', 'repeat' => 10],
);
printf(
    "\nreference=%s return=%d executions=%d vm=%d execute_ex=%d handler=%d\n",
    $referenceResult['status'],
    $referenceResult['execution']['return_value'],
    $referenceResult['execution']['executions'],
    $referenceResult['execution']['vm_handler_calls'],
    $referenceResult['execution']['execute_ex_calls'],
    $referenceResult['execution']['opline_handler_calls'],
);

$temporarySource = <<<'PHP'
<?php
function copied($value): int
{
    return printf("x%s", $value);
}
PHP;

$temporaryResult = native_mir_test_compile_execute(
    $temporarySource,
    'w11p-value-echo-temporary.php',
    ['y'],
    ['wave' => 11, 'function' => 'copied', 'repeat' => 10],
);
printf(
    "\ntemporary=%s return=%d executions=%d vm=%d execute_ex=%d handler=%d\n",
    $temporaryResult['status'],
    $temporaryResult['execution']['return_value'],
    $temporaryResult['execution']['executions'],
    $temporaryResult['execution']['vm_handler_calls'],
    $temporaryResult['execution']['execute_ex_calls'],
    $temporaryResult['execution']['opline_handler_calls'],
);
?>
--EXPECT--
refrefrefrefrefrefrefrefrefref
reference=accepted return=9 executions=10 vm=0 execute_ex=0 handler=0
xyxyxyxyxyxyxyxyxyxy
temporary=accepted return=2 executions=10 vm=0 execute_ex=0 handler=0
