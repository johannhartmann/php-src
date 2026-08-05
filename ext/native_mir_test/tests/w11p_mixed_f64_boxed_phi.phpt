--TEST--
Native top-level ternaries box F64 inputs for mixed zval PHIs
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
$condition = '';
$fallback = 23.5;

return $condition ?: $fallback;
PHP,
    'w11p-mixed-f64-boxed-phi.php',
    [],
    ['wave' => 11, 'repeat' => 10],
);

printf(
    "%s return=%s runs=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['executions'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=23.5 runs=10 active=0
