--TEST--
Native scalar facts crossing catch continuations use frame authority
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
function w11p_catch_scalar_frame_authority(bool $complete): bool
{
    $completed = false;
    try {
        if (!$complete) {
            throw new RuntimeException('expected');
        }
        $completed = true;
    } catch (RuntimeException) {
    }
    return $completed;
}
PHP;

foreach ([false, true] as $complete) {
    $result = native_mir_test_compile_execute(
        $source,
        'w11p-catch-scalar-frame-authority.php',
        [$complete],
        [
            'wave' => 11,
            'function' => 'w11p_catch_scalar_frame_authority',
            'repeat' => 20,
        ],
    );
    printf(
        "%s return=%s vm=%d execute_ex=%d handler=%d active=%d\n",
        $result['status'],
        json_encode($result['execution']['return_value']),
        $result['execution']['vm_handler_calls'],
        $result['execution']['execute_ex_calls'],
        $result['execution']['opline_handler_calls'],
        $result['execution']['entry_active_calls'],
    );
}
?>
--EXPECT--
accepted return=false vm=0 execute_ex=0 handler=0 active=0
accepted return=true vm=0 execute_ex=0 handler=0 active=0
