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
final class W11PStaticOperandObject {
    public static int $value = 0;
}
class W11PStaticOperandBase {
    public static int $value = 0;
    public static function assign(): int {
        static::$value = 40;
        static::$value += 2;
        return static::$value;
    }
}
final class W11PStaticOperandChild extends W11PStaticOperandBase {}
function w11p_static_literal_operands(): int {
    W11PStaticOperandObject::$value = 10;
    $reference =& W11PStaticOperandObject::$value;
    $reference += 10;
    $before = W11PStaticOperandObject::$value++;
    $after = ++W11PStaticOperandObject::$value;
    W11PStaticOperandObject::$value += 20;
    return $before === 20
        && $after === 22
        && isset(W11PStaticOperandObject::$value)
        && !empty(W11PStaticOperandObject::$value)
        ? W11PStaticOperandObject::$value
        : 0;
}
function w11p_static_dynamic_operands(): int {
    $class = W11PStaticOperandObject::class;
    $name = 'value';
    $class::${$name} = 40;
    $class::${$name} += 2;
    return $class::${$name};
}
function w11p_static_late_bound_operands(): int {
    return W11PStaticOperandChild::assign();
}
PHP;

foreach ([
    'w11p_static_literal_operands',
    'w11p_static_dynamic_operands',
    'w11p_static_late_bound_operands',
] as $function) {
    $result = native_mir_test_compile_execute(
        $source,
        'w11p-explicit-static-property-operands.php',
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
w11p_static_literal_operands accepted return=42 vm=0 execute_ex=0 handler=0
w11p_static_dynamic_operands accepted return=42 vm=0 execute_ex=0 handler=0
w11p_static_late_bound_operands accepted return=42 vm=0 execute_ex=0 handler=0
