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
class NativeClassOperandBase {
    public const BASE = 40;
}
class NativeClassOperandChild extends NativeClassOperandBase {
    public const OFFSET = 2;
    public static function scopedNames(): array {
        return [self::class, parent::class, static::class];
    }
}
final class NativeClassOperandLeaf extends NativeClassOperandChild {}
function native_class_literal_constant(): int {
    return NativeClassOperandLeaf::BASE + NativeClassOperandLeaf::OFFSET;
}
function native_class_dynamic_constant(): int {
    $class = NativeClassOperandLeaf::class;
    $base = 'BASE';
    $offset = 'OFFSET';
    return $class::{$base} + $class::{$offset};
}
function native_class_dynamic_fetch(): int {
    $class = NativeClassOperandLeaf::class;
    $object = new NativeClassOperandLeaf();
    return $object instanceof $class ? 42 : 0;
}
function native_class_names(): int {
    $object = new NativeClassOperandLeaf();
    $names = NativeClassOperandLeaf::scopedNames();
    return get_class($object) === NativeClassOperandLeaf::class
        && $object::class === NativeClassOperandLeaf::class
        && $names === [
            NativeClassOperandChild::class,
            NativeClassOperandBase::class,
            NativeClassOperandLeaf::class,
        ]
        ? 42
        : 0;
}
PHP;

foreach ([
    'native_class_literal_constant',
    'native_class_dynamic_constant',
    'native_class_dynamic_fetch',
    'native_class_names',
] as $function) {
    $result = native_mir_test_compile_execute(
        $source,
        'explicit-class-operands.php',
        [],
        ['function' => $function],
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
native_class_literal_constant accepted return=42 vm=0 execute_ex=0 handler=0
native_class_dynamic_constant accepted return=42 vm=0 execute_ex=0 handler=0
native_class_dynamic_fetch accepted return=42 vm=0 execute_ex=0 handler=0
native_class_names accepted return=42 vm=0 execute_ex=0 handler=0
