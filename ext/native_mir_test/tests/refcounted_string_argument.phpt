--TEST--
Native typed calls consume refcounted string arguments
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
function native_string_argument_leaf(string $value): int
{
    return strlen($value);
}

final class NativeStringLifetimeProbe
{
    public static int $alive = 0;

    public function __construct()
    {
        self::$alive++;
    }

    public function __destruct()
    {
        self::$alive--;
    }
}

function native_string_argument_root(int $count): array
{
    $value = 0;
    $probe = new NativeStringLifetimeProbe();
    $text = str_repeat('native-', 2) . $count;
    $alias = $text;
    for ($index = 0; $index < $count; $index++) {
        $value += native_string_argument_leaf($text);
    }
    $alias[0] = 'N';
    unset($probe);
    return [$value, $text, $alias, NativeStringLifetimeProbe::$alive];
}
PHP,
    'refcounted-string-argument.php',
    [20],
    [
        'function' => 'native_string_argument_root',
        'repeat' => 5,
    ],
);

printf(
    "%s return=%s runs=%d closure=%s active=%d lifetime_leaks=%d\n",
    $result['status'],
    json_encode(array_slice($result['execution']['return_value'], 0, 3)),
    $result['execution']['executions'],
    ($result['execution']['failed_codeunits'] ?? -1) === 0
        && ($result['execution']['performance']['ready_codeunits'] ?? -1)
            === ($result['execution']['performance']['compiled_codeunits'] ?? -2)
        ? 'ready'
        : 'incomplete',
    $result['execution']['entry_active_calls'],
    $result['execution']['return_value'][3],
);
?>
--EXPECT--
accepted return=[320,"native-native-20","Native-native-20"] runs=5 closure=ready active=0 lifetime_leaks=0
