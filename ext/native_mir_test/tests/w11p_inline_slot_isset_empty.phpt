--TEST--
Native baseline evaluates simple slot isset and empty directly
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
function inline_slot_state($value)
{
    return [isset($value), empty($value)];
}

function inline_reference_state()
{
    $base = null;
    $value =& $base;
    $null = [isset($value), empty($value)];
    $base = "native";
    return [$null, [isset($value), empty($value)]];
}

function inline_slot_branch_loop($iterations)
{
    $sum = 0;
    $value = 1;
    for ($i = 0; $i < $iterations; $i++) {
        if (isset($value) && !empty($value)) {
            $sum++;
        }
    }
    return $sum;
}

function inline_slot_states()
{
    return [
        inline_slot_state(null),
        inline_slot_state(false),
        inline_slot_state(true),
        inline_slot_state(0),
        inline_slot_state(7),
        inline_slot_state(0.0),
        inline_slot_state(1.5),
        inline_slot_state(""),
        inline_slot_state("0"),
        inline_slot_state("native"),
        inline_slot_state([]),
        inline_slot_state([1]),
        inline_slot_state(new stdClass()),
        inline_reference_state(),
        inline_slot_branch_loop(50),
    ];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-inline-slot-isset-empty.php',
    [],
    [
        'wave' => 11,
        'function' => 'inline_slot_states',
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
accepted return=[[false,true],[true,true],[true,false],[true,true],[true,false],[true,true],[true,false],[true,true],[true,true],[true,false],[true,true],[true,false],[true,false],[[false,true],[true,false]],50] vm=0 execute_ex=0 handler=0 active=0
