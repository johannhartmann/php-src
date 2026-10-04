--TEST--
Native nested Elvis branches preserve their register result aliases
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
function native_nested_elvis(int $first, int $second, int $third): int
{
    return $first ?: $second ?: $third;
}

function native_nested_elvis_root(): array
{
    return [
        native_nested_elvis(1, 2, 3),
        native_nested_elvis(0, 2, 3),
        native_nested_elvis(0, 0, 3),
    ];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'nested-elvis-result.php',
    [],
    [
        'function' => 'native_nested_elvis_root',
        'repeat' => 20,
    ],
);

printf(
    "%s return=%s vm=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['vm_handler_calls'],
);
?>
--EXPECT--
accepted return=[1,2,3] vm=0
