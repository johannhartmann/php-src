--TEST--
Native scalar ZVAL loop PHIs retain register backedge definitions
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
function seeded_scalar_zval_loop_phi(mixed $seed): string
{
    for ($count = 0; $count < 1; $count++) {
        $index = $seed;
        for ($index = 0; $index < 1;) {
            for (; $index < 1; $index++) {
            }
        }
        for ($index = 0; $index < 1; $index++) {
        }
    }
    return 'ok';
}

function cyclic_scalar_zval_loop_phi(): string
{
    for ($count = 0; $count < 1; $count++) {
        for ($index = 0; $index < 1;) {
            for (; $index < 1; $index++) {
            }
        }
    }
    return 'ok';
}
PHP;

foreach (
    [
        ['seeded', 'seeded_scalar_zval_loop_phi', [null]],
        ['cyclic', 'cyclic_scalar_zval_loop_phi', []],
    ] as [$label, $function, $arguments]
) {
    $result = native_mir_test_compile_execute(
        $source,
        "w14-scalar-zval-loop-phi-{$label}.php",
        $arguments,
        [
            'wave' => 11,
            'function' => $function,
            'repeat' => 20,
        ],
    );
    printf(
        "%s %s=%s vm=%d active=%d closure=%s\n",
        $result['status'],
        $label,
        json_encode($result['execution']['return_value']),
        $result['execution']['vm_handler_calls'],
        $result['execution']['entry_active_calls'],
        ($result['execution']['failed_codeunits'] ?? -1) === 0
            && ($result['execution']['performance']['ready_codeunits'] ?? -1)
                === ($result['execution']['performance']['compiled_codeunits'] ?? -2)
            ? 'ready'
            : 'incomplete',
    );
}
?>
--EXPECT--
accepted seeded="ok" vm=0 active=0 closure=ready
accepted cyclic="ok" vm=0 active=0 closure=ready
