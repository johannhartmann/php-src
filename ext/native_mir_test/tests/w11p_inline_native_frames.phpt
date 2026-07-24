--TEST--
Native baseline builds scalar native-to-native frames in generated code
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
function inline_zero()
{
    return 1;
}

function inline_identity($value)
{
    return $value;
}

function inline_first($first, $second)
{
    return $first;
}

function inline_root()
{
    inline_zero();
    $integer = inline_identity(42);
    $boolean = inline_identity(true);
    $float = inline_identity(2.5);
    $null = inline_identity(null);
    $first = inline_first(7, 9);
    return $integer + $first
        + ($boolean ? 1 : 0)
        + (int) $float
        + ($null === null ? 1 : 0);
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-inline-native-frames.php',
    [],
    [
        'wave' => 11,
        'function' => 'inline_root',
        'repeat' => 10,
    ],
);
printf(
    "%s return=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=53 vm=0 execute_ex=0 handler=0 active=0
