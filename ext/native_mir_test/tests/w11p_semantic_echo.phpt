--TEST--
Native baseline executes semantic scalar echo without projection carriers
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
function selected(): int
{
    echo null;
    echo false;
    echo true;
    echo 42;
    echo 1.5;
    return 7;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-semantic-echo.php',
    [],
    ['wave' => 11, 'function' => 'selected'],
);

printf(
    "%s return=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
);
?>
--EXPECT--
1421.5accepted return=7 vm=0 execute_ex=0 handler=0
