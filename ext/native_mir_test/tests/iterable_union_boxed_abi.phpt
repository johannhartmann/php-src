--TEST--
Native typed components keep iterable union arguments boxed
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
function native_iterable_generator(): Generator
{
    yield 1;
}

function native_iterable_leaf(iterable $value): string
{
    return get_debug_type($value);
}

function native_iterable_root(): array
{
    return [
        native_iterable_leaf([1]),
        native_iterable_leaf(native_iterable_generator()),
        native_iterable_leaf(new ArrayIterator([1])),
    ];
}
PHP,
    'iterable-union-boxed-abi.php',
    [],
    [
        'function' => 'native_iterable_root',
        'repeat' => 20,
    ],
);

printf(
    "%s return=%s active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=["array","Generator","ArrayIterator"] active=0
