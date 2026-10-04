--TEST--
Native MIR leaves unselected generator codeunits unseen
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
function native_static_generator(): Generator {
    yield 21;
}
function native_reserve_generators(): int {
    eval('function native_eval_generator(): Generator { yield 42; }');
    return 42;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'suspendable-reservations.php',
    [],
    ['function' => 'native_reserve_generators'],
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
