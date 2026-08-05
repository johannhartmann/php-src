--TEST--
Native baseline preserves NAN boolean conversion semantics
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
function nan_boolean_semantics(float $value): array
{
    $direct = false;
    if ($value) $direct = true;
    $not = !$value;
    $and = $value && true;
    $or = $value || false;
    return [$direct, $not, $and, $or];
}
PHP,
    'w11p-nan-boolean-semantics.php',
    [NAN],
    [
        'wave' => 11,
        'function' => 'nan_boolean_semantics',
        'repeat' => 1,
    ],
);
printf(
    "%s return=%s vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECTF--
Warning: unexpected NAN value was coerced to bool in w11p-nan-boolean-semantics.php on line 5

Warning: unexpected NAN value was coerced to bool in w11p-nan-boolean-semantics.php on line 6

Warning: unexpected NAN value was coerced to bool in w11p-nan-boolean-semantics.php on line 7

Warning: unexpected NAN value was coerced to bool in w11p-nan-boolean-semantics.php on line 8
accepted return=[true,false,true,true] vm=0 execute_ex=0 handler=0 active=0
