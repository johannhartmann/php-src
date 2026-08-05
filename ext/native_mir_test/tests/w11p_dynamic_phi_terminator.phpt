--TEST--
Native dynamic PHI components retain boxed branch conditions
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
function w11p_dynamic_phi_terminator(): array
{
    $name = 'shadow';
    $shadow = 1;
    $dynamic = $$name;

    $index = 3;
    $key = 'b';
    $values = [];
    while ($index > 0) {
        $key .= 'a';
        $values[$key] = $index;
        $index--;
    }

    $index = 3;
    $key = 'b';
    $sum = 0;
    while ($index > 0) {
        $key .= 'a';
        if ($values[$key]) {
            $sum += $values[$key];
        }
        $index--;
    }

    return [$dynamic, $sum, count($values)];
}
PHP,
    'w11p-dynamic-phi-terminator.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_dynamic_phi_terminator',
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
accepted return=[1,6,3] closure=ready active=0
