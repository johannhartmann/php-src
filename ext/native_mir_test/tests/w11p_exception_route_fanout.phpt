--TEST--
Native source helpers from one opcode share protected exception routes
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
function w11p_exception_route_fanout(array $values, string $selector): array
{
    try {
        [$first] = $values;
        $matched = match ($selector) {
            'one' => 1,
            'two' => 2,
            default => 0,
        };

        return [$first, $matched, count($values)];
    } catch (Throwable) {
        return [-1, -1, -1];
    }
}
PHP,
    'w11p-exception-route-fanout.php',
    [[7, 8], 'two'],
    [
        'wave' => 11,
        'function' => 'w11p_exception_route_fanout',
        'repeat' => 20,
    ],
);
printf(
    "%s return=%s closure=%s active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    ($result['execution']['failed_codeunits'] ?? -1) === 0
        && ($result['execution']['performance']['ready_codeunits'] ?? -1)
            === ($result['execution']['performance']['compiled_codeunits'] ?? -2)
        ? 'ready'
        : 'incomplete',
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=[7,2,2] closure=ready active=0
