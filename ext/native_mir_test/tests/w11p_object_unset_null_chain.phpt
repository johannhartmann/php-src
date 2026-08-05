--TEST--
Native object unset ignores null and uninitialized nested receivers
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
class W11pUnsetContainer
{
    public W11pUnsetContainer $typed;
}

function w11p_object_unset_null_chain(): array
{
    $dynamic = new stdClass();
    unset($dynamic->missing->nested->value);

    $typed = new W11pUnsetContainer();
    unset($typed->typed->nested->value);

    return [(array) $dynamic, isset($typed->typed)];
}
PHP,
    'w11p-object-unset-null-chain.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_object_unset_null_chain',
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
accepted return=[{"missing":null},false] vm=0 execute_ex=0 handler=0 active=0
