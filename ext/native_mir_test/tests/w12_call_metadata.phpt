--TEST--
Native dynamic calls execute argument metadata and preserve reference lvalues
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
function w12_metadata_callee(&$value): void
{
    $value += 5;
}
function w12_metadata_defaults($first = 10, $second = 20, $third = 30): array
{
    return [$first, $second, $third];
}
function w12_metadata_numeric($callable): int
{
    $values = [2];
    $callable($values[0]);
    return $values[0];
}
function w12_metadata_named_reference($callable): int
{
    $values = [2];
    $callable(value: $values[0]);
    return $values[0];
}
function w12_metadata_named_defaults($callable): array
{
    return $callable(third: 3, first: 1);
}
PHP;

$cases = [
    ['numeric', 'w12_metadata_numeric', ['w12_metadata_callee']],
    ['named-reference', 'w12_metadata_named_reference', ['w12_metadata_callee']],
    ['named-defaults', 'w12_metadata_named_defaults', ['w12_metadata_defaults']],
];

foreach ($cases as [$name, $function, $arguments]) {
    $result = native_mir_test_compile_execute(
        $source,
        "w12-call-metadata-$name.php",
        $arguments,
        ['wave' => 11, 'function' => $function],
    );
    printf(
        "%s status=%s result=%s vm=%d execute_ex=%d handler=%d\n",
        $name,
        $result['status'],
        json_encode($result['execution']['return_value'] ?? null),
        $result['execution']['vm_handler_calls'] ?? -1,
        $result['execution']['execute_ex_calls'] ?? -1,
        $result['execution']['opline_handler_calls'] ?? -1,
    );
}
?>
--EXPECT--
numeric status=accepted result=7 vm=0 execute_ex=0 handler=0
named-reference status=accepted result=7 vm=0 execute_ex=0 handler=0
named-defaults status=accepted result=[1,20,3] vm=0 execute_ex=0 handler=0
