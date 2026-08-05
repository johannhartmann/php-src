--TEST--
Native unset dimension does not promote a null container to an array
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
function unset_dim_null_no_promotion_root(): array
{
    $container = null;
    unset($container['missing']);
    return [$container, get_debug_type($container)];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-unset-dim-null-no-promotion.php',
    [],
    [
        'wave' => 11,
        'function' => 'unset_dim_null_no_promotion_root',
        'repeat' => 20,
    ],
);
printf(
    "%s return=%s runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=[null,"null"] runs=20 vm=0 execute_ex=0 handler=0 active=0
