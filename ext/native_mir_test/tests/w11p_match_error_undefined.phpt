--TEST--
Native MATCH_ERROR does not repeat the undefined subject warning
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
function w11p_match_error_undefined(): string
{
    try {
        return match ($undefined) {
            1 => 'one',
        };
    } catch (UnhandledMatchError $exception) {
        return $exception->getMessage();
    }
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-match-error-undefined.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_match_error_undefined',
        'repeat' => 1,
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
Warning: Undefined variable $undefined in w11p-match-error-undefined.php on line 6
accepted return="Unhandled match case NULL" runs=1 vm=0 execute_ex=0 handler=0 active=0
