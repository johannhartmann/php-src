--TEST--
Native component keeps exact PHP pointers in TPDE values across CFG merges
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
function w14_string_pointer_phi(
    string $left,
    string $right,
    bool $chooseLeft,
): bool {
    $selected = $chooseLeft ? $left : $right;
    return $selected === $left;
}

function w14_array_pointer_phi(
    array $left,
    array $right,
    bool $chooseLeft,
): int {
    $selected = $chooseLeft ? $left : $right;
    return $selected[0];
}

function w14_object_pointer_phi(
    object $left,
    object $right,
    bool $chooseLeft,
): int {
    $selected = $chooseLeft ? $left : $right;
    return $selected->value;
}

function w14_register_pointer_phi_root(): array
{
    $left = (object) ['value' => 41];
    $right = (object) ['value' => 42];
    return [
        w14_string_pointer_phi('left', 'right', true),
        w14_string_pointer_phi('left', 'right', false),
        w14_array_pointer_phi([41], [42], false),
        w14_object_pointer_phi($left, $right, true),
    ];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w14-register-pointer-phis.php',
    [],
    [
        'wave' => 11,
        'function' => 'w14_register_pointer_phi_root',
        'repeat' => 20,
    ],
);
$execution = $result['execution'];
printf(
    "%s return=%s runs=%d codeunits=%d components=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    json_encode($execution['return_value']),
    $execution['executions'],
    $execution['native_codeunits'],
    $execution['native_components'],
    $execution['vm_handler_calls'],
    $execution['execute_ex_calls'],
    $execution['opline_handler_calls'],
);
?>
--EXPECT--
accepted return=[true,false,42,41] runs=20 codeunits=4 components=1 vm=0 execute_ex=0 handler=0
