--TEST--
Native baseline preserves refcounted assignment and move ownership
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
function assignment_string($suffix)
{
    return "native" . $suffix;
}

function inline_refcounted_assignments()
{
    $source = assignment_string("-source");
    $copy = $source;

    $target = assignment_string("-old");
    $old_alias = $target;
    $target = $source;
    $expression = ($target = $copy);

    $moved = assignment_string("-moved");
    $condition = strlen($source) > 0;
    $selected = $condition ? $source : $moved;

    $array = [1, 2, 3];
    $array_copy = $array;
    $array_expression = ($array_copy = $array);

    $base = assignment_string("-reference");
    $reference =& $base;
    $from_reference = $reference;
    $target_reference =& $target;
    $target_reference = $base;

    return [
        $source,
        $copy,
        $old_alias,
        $target,
        $expression,
        $moved,
        $selected,
        $array,
        $array_copy,
        $array_expression,
        $base,
        $from_reference,
    ];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-inline-refcounted-assignments.php',
    [],
    [
        'wave' => 11,
        'function' => 'inline_refcounted_assignments',
        'repeat' => 10,
    ],
);
printf(
    "%s return=%s vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=["native-source","native-source","native-old","native-reference","native-source","native-moved","native-source",[1,2,3],[1,2,3],[1,2,3],"native-reference","native-reference"] vm=0 execute_ex=0 handler=0 active=0
