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
function w14_iterable_generator(): Generator
{
    yield 1;
}

function w14_iterable_leaf(iterable $value): string
{
    return get_debug_type($value);
}

function w14_iterable_root(): array
{
    return [
        w14_iterable_leaf([1]),
        w14_iterable_leaf(w14_iterable_generator()),
        w14_iterable_leaf(new ArrayIterator([1])),
    ];
}
PHP,
    'w14-iterable-union-boxed-abi.php',
    [],
    [
        'wave' => 11,
        'function' => 'w14_iterable_root',
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
