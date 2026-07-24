--TEST--
Native class and class-constant operations consume explicit operands
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
class W11PClassOperandBase {
    public const BASE = 40;
}
class W11PClassOperandChild extends W11PClassOperandBase {
    public const OFFSET = 2;
    public static function scopedNames(): array {
        return [self::class, parent::class, static::class];
    }
}
final class W11PClassOperandLeaf extends W11PClassOperandChild {}
function w11p_class_literal_constant(): int {
    return W11PClassOperandLeaf::BASE + W11PClassOperandLeaf::OFFSET;
}
function w11p_class_dynamic_constant(): int {
    $class = W11PClassOperandLeaf::class;
    $base = 'BASE';
    $offset = 'OFFSET';
    return $class::{$base} + $class::{$offset};
}
function w11p_class_dynamic_fetch(): int {
    $class = W11PClassOperandLeaf::class;
    $object = new W11PClassOperandLeaf();
    return $object instanceof $class ? 42 : 0;
}
function w11p_class_names(): int {
    $object = new W11PClassOperandLeaf();
    $names = W11PClassOperandLeaf::scopedNames();
    return get_class($object) === W11PClassOperandLeaf::class
        && $object::class === W11PClassOperandLeaf::class
        && $names === [
            W11PClassOperandChild::class,
            W11PClassOperandBase::class,
            W11PClassOperandLeaf::class,
        ]
        ? 42
        : 0;
}
PHP;

foreach ([
    'w11p_class_literal_constant',
    'w11p_class_dynamic_constant',
    'w11p_class_dynamic_fetch',
    'w11p_class_names',
] as $function) {
    $result = native_mir_test_compile_execute(
        $source,
        'w11p-explicit-class-operands.php',
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
w11p_class_literal_constant accepted return=42 vm=0 execute_ex=0 handler=0
w11p_class_dynamic_constant accepted return=42 vm=0 execute_ex=0 handler=0
w11p_class_dynamic_fetch accepted return=42 vm=0 execute_ex=0 handler=0
w11p_class_names accepted return=42 vm=0 execute_ex=0 handler=0
