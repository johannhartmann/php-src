--TEST--
Native partial applications preserve assignments through by-reference parameters
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
function native_partial_set_reference(&$value): void
{
    $value = 42;
}

function native_partial_reference_assignment(): int
{
    $values = [1];
    $alias =& $values[0];
    $partial = native_partial_set_reference(?);
    $partial($alias);

    return $values[0] + $alias;
}
PHP,
    'partial-by-reference-assignment.php',
    [],
    [
        'function' => 'native_partial_reference_assignment',
        'repeat' => 20,
    ],
);
printf(
    "%s return=%d runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=84 runs=20 vm=0 execute_ex=0 handler=0 active=0
