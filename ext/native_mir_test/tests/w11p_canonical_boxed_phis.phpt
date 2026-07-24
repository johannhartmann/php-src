--TEST--
Native baseline proves canonical boxed PHIs across diamonds and loops
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
function canonical_boxed_phi(bool $chooseLeft, int $iterations): string
{
    $value = "seed";
    if ($chooseLeft) {
        $value = "left";
    } else {
        $value = "right";
    }

    $index = 0;
    while ($index < $iterations) {
        $value .= ":" . $index;
        $index++;
    }
    return $value;
}

function canonical_boxed_phi_root(): array
{
    return [
        canonical_boxed_phi(true, 3),
        canonical_boxed_phi(false, 2),
    ];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-canonical-boxed-phis.php',
    [],
    [
        'wave' => 11,
        'function' => 'canonical_boxed_phi_root',
        'repeat' => 10,
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
accepted return=["left:0:1:2","right:0:1"] runs=10 vm=0 execute_ex=0 handler=0 active=0
