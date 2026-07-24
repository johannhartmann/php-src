--TEST--
Native baseline compares shared strings directly and preserves content equality
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
function string_identity(
    string $value,
    string $same,
    string $equal,
): array {
    return [
        $value === $same,
        $value === $equal,
        $value !== $same,
        $value !== $equal,
    ];
}
PHP;

$value = implode('', ['na', 'tive']);
$equal = implode('', ['na', 'tive']);
$result = native_mir_test_compile_execute(
    $source,
    'w11p-inline-string-identity.php',
    [$value, $value, $equal],
    [
        'wave' => 11,
        'function' => 'string_identity',
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
accepted return=[true,true,false,false] runs=10 vm=0 execute_ex=0 handler=0 active=0
