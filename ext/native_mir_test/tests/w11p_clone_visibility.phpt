--TEST--
Native clone operations enforce method visibility
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
final class W11PProtectedClone {
    protected function __clone() {}
}

final class W11PPrivateClone {
    private function __clone() {}
}

function w11p_clone_protected_visibility(): int {
    try {
        clone new W11PProtectedClone();
        return 0;
    } catch (Error $error) {
        return 1;
    }
}

function w11p_clone_private_visibility(): int {
    try {
        clone new W11PPrivateClone();
        return 0;
    } catch (Error $error) {
        return 1;
    }
}
PHP;

foreach ([
    'w11p_clone_protected_visibility',
    'w11p_clone_private_visibility',
] as $function) {
    $result = native_mir_test_compile_execute(
        $source,
        'w11p-clone-visibility.php',
        [],
        ['wave' => 11, 'function' => $function],
    );
    printf(
        "%s %s return=%s vm=%d execute_ex=%d handler=%d\n",
        $function,
        $result['status'],
        json_encode($result['execution']['return_value'] ?? null),
        $result['execution']['vm_handler_calls'] ?? -1,
        $result['execution']['execute_ex_calls'] ?? -1,
        $result['execution']['opline_handler_calls'] ?? -1,
    );
}
?>
--EXPECT--
w11p_clone_protected_visibility accepted return=1 vm=0 execute_ex=0 handler=0
w11p_clone_private_visibility accepted return=1 vm=0 execute_ex=0 handler=0
