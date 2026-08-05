--TEST--
Native irreducible control flow executes every cycle entry
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$results = [];
foreach ([0, 3] as $input) {
    $result = native_mir_test_compile_execute(
        <<<'PHP'
<?php
function irreducible_control_flow(int $value): int
{
    $steps = 0;
    if ($value > 0) {
        goto left;
    }
right:
    $steps++;
    $value--;
    if ($steps >= 4) {
        goto done;
    }
left:
    $value++;
    if ($value < 2) {
        goto right;
    }
done:
    return $value;
}
PHP,
        'w11p-irreducible-control-flow.php',
        [$input],
        [
            'wave' => 11,
            'function' => 'irreducible_control_flow',
            'repeat' => 2,
        ],
    );
    $results[] = [
        $result['status'],
        $result['execution']['return_value'],
        $result['execution']['vm_handler_calls'],
        $result['execution']['execute_ex_calls'],
        $result['execution']['opline_handler_calls'],
        $result['execution']['entry_active_calls'],
    ];
}
echo json_encode($results), "\n";
?>
--EXPECT--
[["accepted",-1,0,0,0,0],["accepted",4,0,0,0,0]]
