--TEST--
Native baseline executes return type verification as semantic MIR
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
function compileReturn(string $source, string $filename, string $function): array
{
    return native_mir_test_compile_execute(
        $source,
        $filename,
        [],
        ['wave' => 11, 'function' => $function],
    );
}

$exact = compileReturn(
    '<?php function exactReturn(): int { return 7; }',
    'return-exact.php',
    'exactReturn',
);
$coerced = compileReturn(
    '<?php function coercedReturn(): int { return true; }',
    'return-coerced.php',
    'coercedReturn',
);
$nullable = compileReturn(
    '<?php function nullableReturn(): ?int { return null; }',
    'return-nullable.php',
    'nullableReturn',
);

printf(
    "exact=%s:%d coerced=%s:%d nullable=%s:%s vm=%d/%d/%d\n",
    $exact['status'],
    $exact['execution']['return_value'],
    $coerced['status'],
    $coerced['execution']['return_value'],
    $nullable['status'],
    get_debug_type($nullable['execution']['return_value']),
    $exact['execution']['vm_handler_calls'],
    $coerced['execution']['vm_handler_calls'],
    $nullable['execution']['vm_handler_calls'],
);

try {
    compileReturn(
        '<?php declare(strict_types=1); function strictReturn(): int { return 1.5; }',
        'return-strict.php',
        'strictReturn',
    );
    echo "strict=missing-error\n";
} catch (TypeError $error) {
    printf("strict=%s\n", $error->getMessage());
}

$recovered = compileReturn(
    '<?php function recoveredReturn(): int { return 9; }',
    'return-recovered.php',
    'recoveredReturn',
);
printf(
    "recovered=%s:%d vm=%d execute_ex=%d handler=%d\n",
    $recovered['status'],
    $recovered['execution']['return_value'],
    $recovered['execution']['vm_handler_calls'],
    $recovered['execution']['execute_ex_calls'],
    $recovered['execution']['opline_handler_calls'],
);
?>
--EXPECT--
exact=accepted:7 coerced=accepted:1 nullable=accepted:null vm=0/0/0
strict=strictReturn(): Return value must be of type int, float returned
recovered=accepted:9 vm=0 execute_ex=0 handler=0
