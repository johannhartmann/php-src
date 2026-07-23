--TEST--
Native baseline compiles only the selected root and static dependencies
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$source = "<?php\n";
for ($index = 0; $index < 1000; $index++) {
    $source .= "function independent_$index(): int { return $index; }\n";
}
$source .= <<<'PHP'
function selected_root(): int
{
    return independent_777();
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-lazy-codeunits.php',
    [],
    ['wave' => 11, 'function' => 'selected_root'],
);
printf(
    "%s return=%d codeunits=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $result['execution']['return_value'] ?? -1,
    $result['execution']['native_codeunits'] ?? -1,
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
);
?>
--EXPECT--
accepted return=777 codeunits=2 vm=0 execute_ex=0 handler=0
