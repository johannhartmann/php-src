--TEST--
Native boxed internal bool results remain correct in loop conditions
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
function boxed_internal_bool_loop(string $fileName): array
{
    $file = fopen($fileName, 'rb');
    $lines = [];
    while (!feof($file)) {
        $lines[] = fgets($file);
        if (count($lines) === 4) {
            break;
        }
    }
    fclose($file);
    return $lines;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-boxed-internal-bool-loop.php',
    [__FILE__],
    [
        'wave' => 11,
        'function' => 'boxed_internal_bool_loop',
        'repeat' => 20,
    ],
);
$return = $result['execution']['return_value'];
printf(
    "%s lines=%d first=%s active=%d vm=%d execute_ex=%d handlers=%d\n",
    $result['status'],
    count($return),
    trim($return[0]),
    $result['execution']['entry_active_calls'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
);
?>
--EXPECT--
accepted lines=4 first=<?php active=0 vm=0 execute_ex=0 handlers=0
