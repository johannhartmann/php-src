--TEST--
Native baseline iterates guarded packed long arrays directly
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
function inline_packed_foreach($values)
{
    $sum = 0;
    foreach ($values as $value) {
        $sum += (int) $value;
    }
    return $sum;
}
PHP;

$cases = [
    'packed-long' => [[2, 3, 5], 10],
    'packed-string-fallback' => [[2, '3', 5], 10],
    'sparse-fallback' => [[0 => 2, 2 => 5], 7],
    'mixed-fallback' => [['left' => 2, 'right' => 5], 7],
];

foreach ($cases as $name => [$values, $expected]) {
    $result = native_mir_test_compile_execute(
        $source,
        "w11p-inline-packed-foreach-$name.php",
        [$values],
        [
            'wave' => 11,
            'function' => 'inline_packed_foreach',
            'repeat' => 10,
        ],
    );
    printf(
        "%s %s return=%s expected=%d vm=%d execute_ex=%d handler=%d active=%d\n",
        $name,
        $result['status'],
        json_encode($result['execution']['return_value']),
        $expected,
        $result['execution']['vm_handler_calls'],
        $result['execution']['execute_ex_calls'],
        $result['execution']['opline_handler_calls'],
        $result['execution']['entry_active_calls'],
    );
}
?>
--EXPECT--
packed-long accepted return=10 expected=10 vm=0 execute_ex=0 handler=0 active=0
packed-string-fallback accepted return=10 expected=10 vm=0 execute_ex=0 handler=0 active=0
sparse-fallback accepted return=7 expected=7 vm=0 execute_ex=0 handler=0 active=0
mixed-fallback accepted return=7 expected=7 vm=0 execute_ex=0 handler=0 active=0
