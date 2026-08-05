--TEST--
Native internal calls preserve prefer-ref lvalues and literal flags
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
function w11p_internal_prefer_ref_arguments(): array
{
    $primary = [2, 1, 1];
    $secondary = ['two', 'one-b', 'one-a'];
    $ok = array_multisort(
        $primary,
        SORT_ASC,
        SORT_REGULAR,
        $secondary,
        SORT_ASC,
        SORT_STRING,
    );
    return [$ok, $primary, $secondary];
}
PHP,
    'w11p-internal-prefer-ref-arguments.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_internal_prefer_ref_arguments',
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
--EXPECT--
accepted return=[true,[1,1,2],["one-a","one-b","two"]] vm=0 execute_ex=0 handler=0 active=0
