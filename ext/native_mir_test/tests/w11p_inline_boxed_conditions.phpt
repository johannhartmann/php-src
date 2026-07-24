--TEST--
Native baseline branches directly on common boxed values
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
function inline_boxed_truth($value)
{
    if ($value) {
        return 1;
    }
    return 0;
}

function inline_boxed_conditions()
{
    $referenced = "";
    $reference =& $referenced;
    return [
        inline_boxed_truth(null),
        inline_boxed_truth(false),
        inline_boxed_truth(true),
        inline_boxed_truth(0),
        inline_boxed_truth(7),
        inline_boxed_truth(0.0),
        inline_boxed_truth(-0.0),
        inline_boxed_truth(1.5),
        inline_boxed_truth(""),
        inline_boxed_truth("0"),
        inline_boxed_truth("00"),
        inline_boxed_truth("native"),
        inline_boxed_truth([]),
        inline_boxed_truth([0]),
        inline_boxed_truth((object) []),
        inline_boxed_truth($reference),
    ];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-inline-boxed-conditions.php',
    [],
    [
        'wave' => 11,
        'function' => 'inline_boxed_conditions',
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
accepted return=[0,0,1,0,1,0,0,1,0,0,1,1,0,1,1,0] vm=0 execute_ex=0 handler=0 active=0
