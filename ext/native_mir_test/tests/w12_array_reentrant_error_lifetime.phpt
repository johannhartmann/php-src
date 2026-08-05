--TEST--
Native array operations preserve table lifetimes across reentrant error handlers
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
function w12_array_reentrant_error_lifetime(): array
{
    $warnings = 0;
    $array = [false];
    set_error_handler(
        static function (int $code, string $message) use (&$array, &$warnings): bool {
            $warnings++;
            $array = 'clobbered';
            return true;
        }
    );
    $array[0]['key'] = 'value';
    restore_error_handler();
    $first = $array;

    $array = [];
    set_error_handler(
        static function (int $code, string $message) use (&$array, &$warnings): bool {
            $warnings++;
            $array['b'] = 2;
            return true;
        }
    );
    $array['b'] += 1;
    restore_error_handler();
    $compound = $array;

    $array = [];
    set_error_handler(
        static function (int $code, string $message) use (&$array, &$warnings): bool {
            $warnings++;
            $array = 9;
            return true;
        }
    );
    $value = $array[PHP_INT_MAX + 1];
    restore_error_handler();

    return [$first, $compound, $array, $value, $warnings];
}
PHP,
    'w12-array-reentrant-error-lifetime.php',
    [],
    [
        'wave' => 11,
        'function' => 'w12_array_reentrant_error_lifetime',
        'repeat' => 20,
    ],
);

printf(
    "%s return=%s closure=%s vm=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    ($result['execution']['failed_codeunits'] ?? -1) === 0
        && ($result['execution']['performance']['ready_codeunits'] ?? -1)
            === ($result['execution']['performance']['compiled_codeunits'] ?? -2)
        ? 'ready'
        : 'incomplete',
    $result['execution']['vm_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=["clobbered",{"b":2},9,null,4] closure=ready vm=0 active=0
