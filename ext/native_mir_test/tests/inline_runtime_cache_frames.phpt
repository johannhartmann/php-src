--TEST--
Native baseline reuses initialized runtime caches in generated user frames
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
class NativeRuntimeCacheBox
{
    public mixed $value;
}

function native_runtime_cache_leaf($box)
{
    return $box->value;
}

function native_runtime_cache_root()
{
    $box = new NativeRuntimeCacheBox();
    $box->value = 21;
    return native_runtime_cache_leaf($box)
        + native_runtime_cache_leaf($box);
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'inline-runtime-cache-frames.php',
    [],
    [
        'function' => 'native_runtime_cache_root',
        'repeat' => 10,
    ],
);
printf(
    "%s return=%d runs=%d codeunits=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['native_codeunits'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=42 runs=10 codeunits=2 vm=0 execute_ex=0 handler=0 active=0
