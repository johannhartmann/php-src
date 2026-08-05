--TEST--
Native stable dynamic reads preserve SSA without weakening conservative cases
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
function stable_dynamic_read(int $iterations): int
{
    $name = 'value';
    $value = 7;
    $sum = 0;
    for ($index = 0; $index < $iterations; $index++) {
        $sum += $$name;
    }
    return $sum;
}

function reassigned_dynamic_name(bool $alternate): int
{
    $name = 'left';
    if ($alternate) {
        $name = 'right';
    }
    $left = 10;
    $right = 20;
    return $$name;
}

function reassigned_dynamic_target(bool $replace): int
{
    $name = 'value';
    $value = 7;
    if ($replace) {
        $value = 9;
    }
    return $$name;
}

function by_reference_dynamic_read(int &$value): int
{
    $name = 'value';
    return $$name;
}
PHP;

$cases = [
    ['stable_dynamic_read', [500]],
    ['reassigned_dynamic_name', [true]],
    ['reassigned_dynamic_target', [true]],
    ['by_reference_dynamic_read', [42]],
];

foreach ($cases as [$function, $arguments]) {
    $result = native_mir_test_compile_execute(
        $source,
        'w11p-stable-dynamic-read-ssa.php',
        $arguments,
        [
            'wave' => 11,
            'function' => $function,
            'repeat' => $function === 'stable_dynamic_read' ? 20 : 1,
        ],
    );
    printf(
        "%s %s return=%s vm=%d execute_ex=%d handler=%d active=%d\n",
        $function,
        $result['status'],
        json_encode($result['execution']['return_value'] ?? null),
        $result['execution']['vm_handler_calls'] ?? -1,
        $result['execution']['execute_ex_calls'] ?? -1,
        $result['execution']['opline_handler_calls'] ?? -1,
        $result['execution']['entry_active_calls'] ?? -1,
    );
}
?>
--EXPECT--
stable_dynamic_read accepted return=3500 vm=0 execute_ex=0 handler=0 active=0
reassigned_dynamic_name accepted return=20 vm=0 execute_ex=0 handler=0 active=0
reassigned_dynamic_target accepted return=9 vm=0 execute_ex=0 handler=0 active=0
by_reference_dynamic_read accepted return=42 vm=0 execute_ex=0 handler=0 active=0
