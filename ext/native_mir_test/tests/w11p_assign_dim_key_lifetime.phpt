--TEST--
Native dimension assignments release temporary string keys
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
class W11PAssignDimKeyLifetime
{
    private static array $cache = [];

    public static function resolve(int $x, int $y): int
    {
        return self::$cache["$x-$y"] ??= $x + $y;
    }
}

function w11p_assign_dim_key_lifetime(): int
{
    return W11PAssignDimKeyLifetime::resolve(20, 22);
}
PHP,
    'w11p-assign-dim-key-lifetime.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_assign_dim_key_lifetime',
        'repeat' => 20,
    ],
);
printf(
    "%s return=%d codeunits=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['native_codeunits'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=42 codeunits=2 vm=0 execute_ex=0 handler=0 active=0
