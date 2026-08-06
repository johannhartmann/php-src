--TEST--
Native short-circuit result consumes a refcounted temporary
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
function w11p_short_circuit_tmp_lifetime(): int
{
    $value = [1, 2, 3] or throw new Exception('unreachable');
    return count($value);
}

function w11p_short_circuit_string_lifetime(string $input): int
{
    $value = $input . '-suffix' or throw new Exception('unreachable');
    return strlen($value);
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-short-circuit-tmp-lifetime.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_short_circuit_tmp_lifetime',
        'repeat' => 20,
    ],
);

printf(
    "array %s return=%d runs=%d vm=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['entry_active_calls'],
);

$result = native_mir_test_compile_execute(
    $source,
    'w11p-short-circuit-string-lifetime.php',
    ['value'],
    [
        'wave' => 11,
        'function' => 'w11p_short_circuit_string_lifetime',
        'repeat' => 20,
    ],
);

printf(
    "string %s return=%d runs=%d vm=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
array accepted return=3 runs=20 vm=0 active=0
string accepted return=12 runs=20 vm=0 active=0
