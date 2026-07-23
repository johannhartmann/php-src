--TEST--
Native MIR W11 leaves unselected generator codeunits unseen
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
function w11_static_generator(): Generator {
    yield 21;
}
function w11_reserve_generators(): int {
    eval('function w11_eval_generator(): Generator { yield 42; }');
    return 42;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11-suspendable-reservations.php',
    [],
    ['wave' => 11, 'function' => 'w11_reserve_generators'],
);
printf(
    "%s return=%d units=%d reserved=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $result['execution']['return_value'] ?? -1,
    $result['execution']['native_codeunits'] ?? -1,
    $result['execution']['suspendable_reserved'] ?? -1,
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
);
?>
--EXPECT--
accepted return=42 units=2 reserved=0 vm=0 execute_ex=0 handler=0
