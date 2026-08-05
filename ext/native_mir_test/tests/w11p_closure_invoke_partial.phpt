--TEST--
Native Closure __invoke partial applications preserve their temporary handler lifetime
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
function w11p_closure_invoke_partial(int $right): int
{
    $closure = static function (int $left, int $right): int {
        return $left + $right;
    };
    $partial = $closure->__invoke(4, ?);

    return $partial($right);
}
PHP,
    'w11p-closure-invoke-partial.php',
    [6],
    [
        'wave' => 11,
        'function' => 'w11p_closure_invoke_partial',
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
accepted return=10 closure=ready active=0
