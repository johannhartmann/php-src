--TEST--
Native object return keeps its payload register live through addref
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
final class W11PObjectReturnRegisterProbe
{
    public int $value = 42;
}

function object_return_register_lifetime(int $depth): W11PObjectReturnRegisterProbe
{
    $root = new W11PObjectReturnRegisterProbe();
    $next = [$root];

    for ($level = 1; $level < $depth; $level++) {
        $queue = $next;
        $next = [];
        while (count($queue) > 0) {
            array_shift($queue);
            for ($width = 0; $width < 3; $width++) {
                $next[] = new W11PObjectReturnRegisterProbe();
            }
        }
    }

    return $root;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-object-return-register-lifetime.php',
    [1],
    [
        'wave' => 11,
        'function' => 'object_return_register_lifetime',
        'repeat' => 20,
    ],
);
$return = $result['execution']['return_value'];
printf(
    "%s class=%s value=%d active=%d\n",
    $result['status'],
    $return::class,
    $return->value,
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted class=W11PObjectReturnRegisterProbe value=42 active=0
