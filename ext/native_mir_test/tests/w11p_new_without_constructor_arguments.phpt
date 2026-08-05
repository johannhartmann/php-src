--TEST--
Native NEW without a constructor evaluates arguments through the VM dummy call
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
final class W11PNoConstructor
{
}

function w11p_no_constructor_side_effect(int &$state): int
{
    $state = 42;
    return 1;
}

function w11p_new_without_constructor_arguments(): int
{
    $state = 0;
    $object = new W11PNoConstructor(
        w11p_no_constructor_side_effect($state),
    );
    return $state + ($object instanceof W11PNoConstructor ? 0 : 100);
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-new-without-constructor-arguments.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_new_without_constructor_arguments',
        'repeat' => 10,
    ],
);
$execution = $result['execution'];
printf(
    "%s return=%d runs=%d codeunits=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $execution['return_value'],
    $execution['executions'],
    $execution['native_codeunits'],
    $execution['vm_handler_calls'],
    $execution['execute_ex_calls'],
    $execution['opline_handler_calls'],
    $execution['entry_active_calls'],
);
?>
--EXPECT--
accepted return=42 runs=10 codeunits=2 vm=0 execute_ex=0 handler=0 active=0
