--TEST--
Native baseline publishes mutual recursion as one atomic SCC
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
function is_even(bool $again): bool
{
    if (!$again) {
        return true;
    }
    return is_odd(false);
}

function is_odd(bool $again): bool
{
    if (!$again) {
        return false;
    }
    return is_even(false);
}

PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-recursive-scc.php',
    [true],
    ['wave' => 11, 'function' => 'is_even'],
);
printf(
    "%s return=%s codeunits=%d components=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    var_export($result['execution']['return_value'] ?? null, true),
    $result['execution']['native_codeunits'] ?? -1,
    $result['execution']['native_components'] ?? -1,
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
);
?>
--EXPECT--
accepted return=false codeunits=2 components=1 vm=0 execute_ex=0 handler=0
