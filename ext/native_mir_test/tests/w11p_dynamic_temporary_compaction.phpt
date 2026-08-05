--TEST--
Native MIR optimizes large include codeunits and compacts eval temporaries
--INI--
memory_limit=32M
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$included = __DIR__ . '/w11p_dynamic_temporary_compaction.inc';
$includeProgram = "<?php\n";
for ($index = 0; $index < 1024; $index++) {
    $includeProgram .= '$result = ((1 + 1) * 3) - 2; '
        . 'if ($result !== 4) { throw new Exception("bad include result"); }';
}
$includeProgram .= "return \$result;\n";
file_put_contents($included, $includeProgram);

$source = <<<'PHP'
<?php
function w11p_dynamic_temporary_compaction(string $included): array
{
    $value = 1;
    $program = '';
    for ($index = 0; $index < 256; $index++) {
        $program .= '$result = (($value + 1) * 3) - 2; '
            . 'if ($result !== 4) { throw new Exception("bad result"); }';
    }
    eval($program);
    return [$result, include $included];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-dynamic-temporary-compaction.php',
    [$included],
    ['wave' => 11, 'function' => 'w11p_dynamic_temporary_compaction'],
);
unlink($included);
printf(
    "%s return=%s vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value'] ?? null),
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
);
?>
--CLEAN--
<?php
@unlink(__DIR__ . '/w11p_dynamic_temporary_compaction.inc');
?>
--EXPECT--
accepted return=[4,4] vm=0 execute_ex=0 handler=0
