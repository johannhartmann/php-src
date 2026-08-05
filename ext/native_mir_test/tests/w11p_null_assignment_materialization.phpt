--TEST--
Native exact null assignments materialize the canonical frame slot
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
function null_assignment_materialization(bool $replace): string
{
    $value = null;
    if ($value !== null) {
        return 'bad-initial';
    }
    if ($replace) {
        $value = 'set';
        if ($value === null) {
            return 'bad-set';
        }
        $value = null;
    }
    return $value === null ? 'null' : 'bad-final';
}
PHP;

foreach ([false, true] as $index => $replace) {
    $result = native_mir_test_compile_execute(
        $source,
        "w11p-null-assignment-materialization-$index.php",
        [$replace],
        [
            'wave' => 11,
            'function' => 'null_assignment_materialization',
            'repeat' => 20,
        ],
    );
    printf(
        "%s return=%s vm=%d execute_ex=%d handler=%d active=%d\n",
        $result['status'],
        $result['execution']['return_value'],
        $result['execution']['vm_handler_calls'],
        $result['execution']['execute_ex_calls'],
        $result['execution']['opline_handler_calls'],
        $result['execution']['entry_active_calls'],
    );
}
?>
--EXPECT--
accepted return=null vm=0 execute_ex=0 handler=0 active=0
accepted return=null vm=0 execute_ex=0 handler=0 active=0
