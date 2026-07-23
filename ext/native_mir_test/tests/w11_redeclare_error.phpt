--TEST--
Native MIR W11 preserves fatal redeclaration semantics without VM fallback
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
function w11_redeclare_error(): void {
    eval('function w11_redeclared(): int { return 42; }');
    eval('function w11_redeclared(): int { return 41; }');
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11-redeclare.php',
    [],
    ['wave' => 11, 'function' => 'w11_redeclare_error'],
);
printf(
    "status=%s vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
);
?>
--EXPECTF--

Fatal error: Cannot redeclare function w11_redeclared() (previously declared in %s) in %s on line 1
status=error vm=0 execute_ex=0 handler=0
