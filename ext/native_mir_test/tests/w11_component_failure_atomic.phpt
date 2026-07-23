--TEST--
Native MIR W11 rolls back an entire codeunit component after entry publication fails
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
function w11_atomic_child(int $value): int {
    return $value + 2;
}
function w11_atomic_root(): int {
    return w11_atomic_child(40);
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11-component-failure-atomic.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11_atomic_root',
        'fault' => 'entry_publish_failure',
    ],
);
$diagnostic = end($result['diagnostics']);
$units = $result['execution']['native_codeunits'];
$failed = $result['execution']['failed_codeunits'];
printf(
    "%s phase=%s code=%s atomic=%d units=%d executions=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $result['phase'],
    $diagnostic['code'],
    $failed > 0 && $failed === $units,
    $units,
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
);
?>
--EXPECTF--
error phase=execute code=NATIVE0006 atomic=1 units=%d executions=0 vm=0 execute_ex=0 handler=0
