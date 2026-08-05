--TEST--
Native PHI/Pi lowering preserves float ranges and parallel CFG edges
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
function w11p_float_range_pi(float $value, float $maximum): float
{
    if ($value > $maximum) {
        return $value;
    }
    return $maximum;
}

function w11p_duplicate_predecessor(mixed $value): mixed
{
    $copy = $value;
    if ($copy === false) {
        if (true) {
        }
    }
    return $copy;
}

function w11p_identical_conditional_target(
    mixed $publicIdentifier,
    mixed $systemIdentifier,
    mixed $name,
): array {
    if ($name !== 'html'
        || $publicIdentifier !== null
        || ($systemIdentifier !== null
            && $systemIdentifier !== 'about:legacy-compat')) {
    }
    return [$publicIdentifier, $systemIdentifier, $name];
}

function w11p_match_parallel_edges(int $day): bool
{
    return match ($day) {
        1, 7 => false,
        2, 3, 4, 5, 6 => true,
    };
}
PHP;

$cases = [
    ['w11p_float_range_pi', [4.0, 0.0]],
    ['w11p_duplicate_predecessor', [42]],
    [
        'w11p_identical_conditional_target',
        [null, 'about:legacy-compat', 'html'],
    ],
    ['w11p_match_parallel_edges', [1]],
];
foreach ($cases as [$function, $arguments]) {
    $result = native_mir_test_compile_execute(
        $source,
        "w11p-phi-pi-edge-shapes-$function.php",
        $arguments,
        [
            'wave' => 11,
            'function' => $function,
        ],
    );
    printf(
        "%s return=%s vm=%d execute_ex=%d handler=%d active=%d\n",
        $result['status'],
        json_encode($result['execution']['return_value']),
        $result['execution']['vm_handler_calls'],
        $result['execution']['execute_ex_calls'],
        $result['execution']['opline_handler_calls'],
        $result['execution']['entry_active_calls'],
    );
}
?>
--EXPECT--
accepted return=4 vm=0 execute_ex=0 handler=0 active=0
accepted return=42 vm=0 execute_ex=0 handler=0 active=0
accepted return=[null,"about:legacy-compat","html"] vm=0 execute_ex=0 handler=0 active=0
accepted return=false vm=0 execute_ex=0 handler=0 active=0
