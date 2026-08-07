--TEST--
Native AArch64 iterates boxed values and reuses string foreach keys safely
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
function inline_refcounted_foreach(array $strings, array $complex): array
{
    $stringKeys = [];
    $stringValues = [];
    $key = 'immutable-key';
    $value = 'immutable-value';
    foreach ($strings as $key => $value) {
        $stringKeys[] = $key;
        $stringValues[] = $value;
    }

    $complexKeys = [];
    $complexValues = [];
    foreach ($complex as $key => $value) {
        $complexKeys[] = $key;
        $complexValues[] = $value;
    }

    unset($strings, $complex, $key, $value);

    return [
        $stringKeys,
        $stringValues,
        $complexKeys,
        $complexValues[0],
        $complexValues[1],
        $complexValues[2]->id,
    ];
}
PHP;

$object = (object) ['id' => 7];
$shared = str_repeat('shared', 1);
$strings = [
    str_repeat('alpha', 1) => $shared,
    str_repeat('beta', 1) => $shared,
    str_repeat('gamma', 1) => str_repeat('three', 1),
];
$complex = [
    str_repeat('text', 1) => str_repeat('tpde', 1),
    str_repeat('array', 1) => [2, 3, 5],
    str_repeat('object', 1) => $object,
];
$result = native_mir_test_compile_execute(
    $source,
    'w11p-inline-refcounted-foreach.php',
    [$strings, $complex],
    [
        'wave' => 11,
        'function' => 'inline_refcounted_foreach',
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
accepted return=[["alpha","beta","gamma"],["shared","shared","three"],["text","array","object"],"tpde",[2,3,5],7] runs=30 vm=0 execute_ex=0 handler=0 active=0
