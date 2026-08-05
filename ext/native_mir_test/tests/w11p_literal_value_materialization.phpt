--TEST--
Native literal arrays and strings materialize as request-local owners
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
final class W11PLiteralValueLifetimeProbe
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

function literal_value_materialization(bool $select): array
{
    $literal = ['text' => 'persistent'];
    $copy = $literal;
    $copy['text'] = 'changed';
    $selected = $select ? $literal : [];
    $returned = array_replace([], $selected);
    $length = strlen($returned['text']);
    $probe = new W11PLiteralValueLifetimeProbe();
    return [$literal, $copy, $returned, $length, $probe];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-literal-value-materialization.php',
    [true],
    [
        'wave' => 11,
        'function' => 'literal_value_materialization',
        'repeat' => 20,
    ],
);
$return = $result['execution']['return_value'];
$alias = $return[2];
$return[2]['text'] = 'mutated';
$cow = $alias['text'] === 'persistent' && $return[2]['text'] === 'mutated';
$encoded = json_encode(array_slice($return, 0, 4));
$active = $result['execution']['entry_active_calls'];
$closure = ($result['execution']['failed_codeunits'] ?? -1) === 0
    && ($result['execution']['performance']['ready_codeunits'] ?? -1)
        === ($result['execution']['performance']['compiled_codeunits'] ?? -2)
    ? 'ready'
    : 'incomplete';
unset($alias, $return, $result['execution']['return_value']);
gc_collect_cycles();
printf(
    "%s return=%s closure=%s cow=%s active=%d lifetime_leaks=%d\n",
    $result['status'],
    $encoded,
    $closure,
    $cow ? 'yes' : 'no',
    $active,
    W11PLiteralValueLifetimeProbe::$alive,
);
?>
--EXPECT--
accepted return=[{"text":"persistent"},{"text":"changed"},{"text":"mutated"},10] closure=ready cow=yes active=0 lifetime_leaks=0
