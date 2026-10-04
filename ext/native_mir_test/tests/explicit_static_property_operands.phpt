--TEST--
Native static property operations consume explicit operands
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
final class NativeStaticOperandObject {
    public static int $value = 0;
}
class NativeStaticOperandBase {
    public static int $value = 0;
    public static function assign(): int {
        static::$value = 40;
        static::$value += 2;
        return static::$value;
    }
}
final class NativeStaticOperandChild extends NativeStaticOperandBase {}
function native_static_literal_operands(): int {
    NativeStaticOperandObject::$value = 10;
    $reference =& NativeStaticOperandObject::$value;
    $reference += 10;
    $before = NativeStaticOperandObject::$value++;
    $after = ++NativeStaticOperandObject::$value;
    NativeStaticOperandObject::$value += 20;
    return $before === 20
        && $after === 22
        && isset(NativeStaticOperandObject::$value)
        && !empty(NativeStaticOperandObject::$value)
        ? NativeStaticOperandObject::$value
        : 0;
}
function native_static_dynamic_operands(): int {
    $class = NativeStaticOperandObject::class;
    $name = 'value';
    $class::${$name} = 40;
    $class::${$name} += 2;
    return $class::${$name};
}
function native_static_late_bound_operands(): int {
    return NativeStaticOperandChild::assign();
}
PHP;

foreach ([
    'native_static_literal_operands',
    'native_static_dynamic_operands',
    'native_static_late_bound_operands',
] as $function) {
    $result = native_mir_test_compile_execute(
        $source,
        'explicit-static-property-operands.php',
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
native_static_literal_operands accepted return=42 vm=0 execute_ex=0 handler=0
native_static_dynamic_operands accepted return=42 vm=0 execute_ex=0 handler=0
native_static_late_bound_operands accepted return=42 vm=0 execute_ex=0 handler=0
