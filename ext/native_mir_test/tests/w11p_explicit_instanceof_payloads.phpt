--TEST--
Native instanceof consumes explicit operands and class-fetch payloads
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
class W11PInstanceofBase {}
final class W11PInstanceofChild extends W11PInstanceofBase {
    public static function againstSelf(object $value): bool {
        return $value instanceof self;
    }
    public static function againstParent(object $value): bool {
        return $value instanceof parent;
    }
}
function w11p_instanceof_literal(): bool {
    return new W11PInstanceofChild() instanceof W11PInstanceofBase;
}
function w11p_instanceof_dynamic(): bool {
    $class = W11PInstanceofBase::class;
    return new W11PInstanceofChild() instanceof $class;
}
function w11p_instanceof_self(): bool {
    return W11PInstanceofChild::againstSelf(new W11PInstanceofChild());
}
function w11p_instanceof_parent(): bool {
    return W11PInstanceofChild::againstParent(new W11PInstanceofChild());
}
PHP;

foreach ([
    'w11p_instanceof_literal',
    'w11p_instanceof_dynamic',
    'w11p_instanceof_self',
    'w11p_instanceof_parent',
] as $function) {
    $result = native_mir_test_compile_execute(
        $source,
        'w11p-explicit-instanceof-payloads.php',
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
w11p_instanceof_literal accepted return=true vm=0 execute_ex=0 handler=0
w11p_instanceof_dynamic accepted return=true vm=0 execute_ex=0 handler=0
w11p_instanceof_self accepted return=true vm=0 execute_ex=0 handler=0
w11p_instanceof_parent accepted return=true vm=0 execute_ex=0 handler=0
