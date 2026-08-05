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
function w11p_nested_elvis(int $first, int $second, int $third): int
{
    return $first ?: $second ?: $third;
}

function w11p_nested_elvis_root(): array
{
    return [
        w11p_nested_elvis(1, 2, 3),
        w11p_nested_elvis(0, 2, 3),
        w11p_nested_elvis(0, 0, 3),
    ];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-nested-elvis-result.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_nested_elvis_root',
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
