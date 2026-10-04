--TEST--
Native top-level code keeps compiler-arena runtime caches arena-owned
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
function native_top_code_generator(): Generator
{
    yield 1;
}

function native_top_code_accept(iterable $value): array
{
    return [get_debug_type($value), $value instanceof Generator];
}

return native_top_code_accept(native_top_code_generator());
PHP,
    'top-code-runtime-cache-lifetime.php',
    [],
    [],
);

printf(
    "%s return=%s active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=["Generator",true] active=0
