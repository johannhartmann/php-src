--TEST--
Native MIR preserves source positions and unwind state for include and eval codeunits
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$directory = sys_get_temp_dir() . '/native-source-' . getmypid();
mkdir($directory);
$included = $directory . '/included-source.php';
file_put_contents($included, <<<'PHP'
<?php
function native_dynamic_source_throw(): void {
    $GLOBALS['native_include_unwind'] = native_mir_test_unwind_probe();
    throw new RuntimeException('include-source');
}
PHP);

$source = <<<'PHP'
<?php
function native_dynamic_source_positions(string $included): array {
    try {
        include $included;
        native_dynamic_source_throw();
    } catch (RuntimeException $error) {
        $trace = $error->getTrace();
        $include = [
            basename($error->getFile()) === 'included-source.php',
            $error->getLine() === 4,
            isset($trace[0]['file'])
                && basename($trace[0]['file']) === 'source-positions.php',
            isset($trace[0]['line']) && $trace[0]['line'] === 5,
            ($GLOBALS['native_include_unwind'] ?? 0) > 0,
        ];
    }

    try {
        eval(
            '$GLOBALS["native_eval_unwind"] = native_mir_test_unwind_probe(); '
            . 'throw new RuntimeException("eval-source");'
        );
    } catch (RuntimeException $error) {
        $trace = $error->getTrace();
        $eval = [
            str_contains($error->getFile(), 'source-positions.php('),
            str_ends_with($error->getFile(), " : eval()'d code"),
            $error->getLine() === 1,
            isset($trace[0]['file'])
                && basename($trace[0]['file']) === 'source-positions.php',
            isset($trace[0]['line']) && $trace[0]['line'] === 22,
            ($GLOBALS['native_eval_unwind'] ?? 0) > 0,
        ];
    }
    return [$include, $eval];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'source-positions.php',
    [$included],
    ['function' => 'native_dynamic_source_positions'],
);
if ($result['status'] !== 'accepted') {
    printf("diagnostics=%s\n", json_encode($result['diagnostics'] ?? null));
}
printf(
    "%s return=%s vm=%d execute_ex=%d handler=%d unwind=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value'] ?? null),
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
    $result['execution']['unwind_registered'] ?? -1,
);

unlink($included);
rmdir($directory);
?>
--EXPECT--
accepted return=[[true,true,true,true,true],[true,true,true,true,true,true]] vm=0 execute_ex=0 handler=0 unwind=1
