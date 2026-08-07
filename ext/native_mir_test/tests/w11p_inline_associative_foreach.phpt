--TEST--
Native AArch64 iterates keyed packed, sparse and mixed arrays directly
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
function inline_associative_foreach(array $values): array
{
    $keys = [];
    $sum = 0;
    foreach ($values as $key => $value) {
        $keys[] = $key;
        $sum += $value;
    }
    return [$keys, $sum, $key ?? null, $value ?? null];
}
PHP;

$sparse = [2, 3, 5, 7];
unset($sparse[1]);
$cases = [
    'packed' => [2, 3, 5],
    'sparse' => $sparse,
    'mixed' => ['alpha' => 2, 7 => 3, '' => 5, 'omega' => 7],
];

foreach ($cases as $name => $values) {
    $result = native_mir_test_compile_execute(
        $source,
        "w11p-inline-associative-foreach-$name.php",
        [$values],
        [
            'wave' => 11,
            'function' => 'inline_associative_foreach',
            'repeat' => 10,
        ],
    );
    printf(
        "%s %s return=%s runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
        $name,
        $result['status'],
        json_encode($result['execution']['return_value']),
        $result['execution']['executions'],
        $result['execution']['vm_handler_calls'],
        $result['execution']['execute_ex_calls'],
        $result['execution']['opline_handler_calls'],
        $result['execution']['entry_active_calls'],
    );
}
?>
--EXPECT--
packed accepted return=[[0,1,2],10,2,5] runs=10 vm=0 execute_ex=0 handler=0 active=0
sparse accepted return=[[0,2,3],14,3,7] runs=10 vm=0 execute_ex=0 handler=0 active=0
mixed accepted return=[["alpha",7,"","omega"],17,"omega",7] runs=10 vm=0 execute_ex=0 handler=0 active=0
