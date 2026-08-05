--TEST--
Native ArrayAccess keeps a detached receiver alive across offset warnings
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
class NativeArrayAccessClobber implements ArrayAccess
{
    public function &offsetGet($offset): mixed
    {
        return null;
    }

    public function offsetSet($offset, $value): void {}
    public function offsetUnset($offset): void {}
    public function offsetExists($offset): bool { return false; }
}

function arrayaccess_error_handler_clobbering_root(): mixed
{
    set_error_handler(function (): void {
        $GLOBALS['native_arrayaccess_clobber'] = null;
    });
    $GLOBALS['native_arrayaccess_clobber'] = new NativeArrayAccessClobber();
    $GLOBALS['native_arrayaccess_clobber'][$missing][$missing] = 'value';
    restore_error_handler();
    return $GLOBALS['native_arrayaccess_clobber'];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-arrayaccess-error-handler-clobbering.php',
    [],
    [
        'wave' => 11,
        'function' => 'arrayaccess_error_handler_clobbering_root',
        'repeat' => 20,
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
accepted return=null runs=20 vm=0 execute_ex=0 handler=0 active=0
