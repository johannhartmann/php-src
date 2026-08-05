--TEST--
Native static self-initializers use read semantics for undefined CVs
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
function w11p_static_self_initializer(): int
{
    static $value = $value;
    return $value + 1;
}
PHP,
    'w11p-static-self-initializer.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_static_self_initializer',
        'repeat' => 2,
    ],
);

printf(
    "%s return=%d runs=%d vm=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECTF--
Warning: Undefined variable $value in w11p-static-self-initializer.php on line 4
accepted return=1 runs=2 vm=0 active=0
