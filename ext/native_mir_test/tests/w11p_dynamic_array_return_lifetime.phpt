--TEST--
Native dynamic array return transfers temporary ownership without leaking
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
final class W11PDynamicArrayLifetimeProbe
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

function dynamic_array_return_lifetime(string $value): array
{
    return [
        'size' => strlen($value),
        'upper' => strtoupper($value),
        'probe' => new W11PDynamicArrayLifetimeProbe(),
    ];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-dynamic-array-return-lifetime.php',
    ['native'],
    [
        'wave' => 11,
        'function' => 'dynamic_array_return_lifetime',
        'repeat' => 20,
    ],
);
$return = $result['execution']['return_value'];
$alias = $return;
$return['upper'][0] = 'n';
$cow = $alias['upper'] === 'NATIVE' && $return['upper'] === 'nATIVE';
$size = $alias['size'];
$upper = $alias['upper'];
unset($return, $alias, $result['execution']['return_value']);
gc_collect_cycles();
printf(
    "%s return=%s closure=%s cow=%s active=%d lifetime_leaks=%d\n",
    $result['status'],
    json_encode(['size' => $size, 'upper' => $upper]),
    ($result['execution']['failed_codeunits'] ?? -1) === 0
        && ($result['execution']['performance']['ready_codeunits'] ?? -1)
            === ($result['execution']['performance']['compiled_codeunits'] ?? -2)
        ? 'ready'
        : 'incomplete',
    $cow ? 'yes' : 'no',
    $result['execution']['entry_active_calls'],
    W11PDynamicArrayLifetimeProbe::$alive,
);
?>
--EXPECT--
accepted return={"size":6,"upper":"NATIVE"} closure=ready cow=yes active=0 lifetime_leaks=0
