--TEST--
Native reference assignments invalidate scalar mutation authority
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$cases = [
    'alias' => <<<'PHP'
<?php
function reference_mutation_authority(): array
{
    $value = 0;
    for ($i = 0; $i < 10; $i++) {
        $value += $value;
        $value =& $alias;
        $value += $value;
    }
    return [$value, $alias];
}
PHP,
    'self' => <<<'PHP'
<?php
function reference_mutation_authority(): array
{
    $value = null;
    for ($i = 0; $i < 6; $i++) {
        $copy = ($value =& $value);
        $value = 3 * $value + 0xff000;
        $value += $value;
    }
    return [$value, $copy];
}
PHP,
];

foreach ($cases as $name => $source) {
    $result = native_mir_test_compile_execute(
        $source,
        "w11p-reference-mutation-authority-$name.php",
        [],
        [
            'wave' => 11,
            'function' => 'reference_mutation_authority',
            'repeat' => 10,
        ],
    );
    printf(
        "%s %s %s return=%s runs=%d active=%d\n",
        $name,
        $result['status'],
        $result['execution']['status'],
        json_encode($result['execution']['return_value']),
        $result['execution']['executions'],
        $result['execution']['entry_active_calls'],
    );
}
?>
--EXPECT--
alias accepted returned return=[0,0] runs=10 active=0
self accepted returned return=[19492085760,3248332800] runs=10 active=0
