--TEST--
Native boxed refcounted array reads compose across consumers
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
function array_value_refcounted_consumers(): array
{
    $values = [
        'text' => 'tpde',
        'emptyText' => '',
        'items' => [1, 2, 3],
        'child' => (object) ['id' => 7],
    ];
    $truthy = $values['text'] ? 'yes' : 'no';
    $notText = !$values['text'];
    $notEmptyText = !$values['emptyText'];
    $upper = strtoupper($values['text']);
    $count = count($values['items']);
    $text = $values['text'];
    $child = $values['child'];
    unset($values);

    return [$truthy, $notText, $notEmptyText, $upper, $count, $text, $child->id];
}
PHP,
    'w11p-inline-array-value-refcounted-consumers.php',
    [],
    [
        'wave' => 11,
        'function' => 'array_value_refcounted_consumers',
        'repeat' => 30,
    ],
);
printf(
    "%s return=%s runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=["yes",false,true,"TPDE",3,"tpde",7] runs=30 vm=0 execute_ex=0 handler=0 active=0
