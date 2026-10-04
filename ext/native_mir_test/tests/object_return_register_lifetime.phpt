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
final class NativeObjectReturnRegisterProbe
{
    public int $value = 42;
}

function object_return_register_lifetime(int $depth): NativeObjectReturnRegisterProbe
{
    $root = new NativeObjectReturnRegisterProbe();
    $next = [$root];

    for ($level = 1; $level < $depth; $level++) {
        $queue = $next;
        $next = [];
        while (count($queue) > 0) {
            array_shift($queue);
            for ($width = 0; $width < 3; $width++) {
                $next[] = new NativeObjectReturnRegisterProbe();
            }
        }
    }

    return $root;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'object-return-register-lifetime.php',
    [1],
    [
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
accepted class=NativeObjectReturnRegisterProbe value=42 active=0
