--TEST--
Native MIR W11 preserves object foreach reference property semantics
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$cases = [
    'typed_property' => <<<'PHP'
<?php
class NativeTypedForeachValue
{
    public int $value = 1;
}

function foreach_case()
{
    $object = new NativeTypedForeachValue();
    foreach ($object as &$value) {
        $value = 2;
        try {
            $value = [];
        } catch (TypeError $error) {
            $message = $error->getMessage();
        }
    }
    unset($value);
    return [$object->value, $message];
}
PHP,
    'readonly_property' => <<<'PHP'
<?php
class NativeReadonlyForeachValue
{
    public readonly int $value;

    public function __construct()
    {
        $this->value = 1;
    }
}

function foreach_case()
{
    $object = new NativeReadonlyForeachValue();
    try {
        foreach ($object as &$value) {
        }
    } catch (Error $error) {
        return $error->getMessage();
    }
    return 'no error';
}
PHP,
    'lazy_object' => <<<'PHP'
<?php
class NativeLazyForeachValue
{
    public int $value;
}

function foreach_case()
{
    $initialized = 0;
    $reflector = new ReflectionClass(NativeLazyForeachValue::class);
    $object = $reflector->newLazyGhost(
        function (NativeLazyForeachValue $object) use (&$initialized) {
            $initialized++;
            $object->value = 41;
        },
    );
    $values = [];
    foreach ($object as $key => $value) {
        $values[$key] = $value;
    }
    return [$initialized, $values];
}
PHP,
    'temporary_object' => <<<'PHP'
<?php
class NativeTemporaryForeachValue
{
    private int $value = 1;

    public function collect()
    {
        $values = [];
        foreach ($this as &$value) {
            $value++;
            $values[] = $value;
        }
        unset($value);
        return $values;
    }
}

function foreach_case()
{
    return (new NativeTemporaryForeachValue())->collect();
}
PHP,
];

foreach ($cases as $name => $source) {
    $result = native_mir_test_compile_execute(
        $source,
        "w11p-foreach-object-reference-$name.php",
        [],
        ['wave' => 11, 'function' => 'foreach_case'],
    );
    printf(
        "%s %s %s return=%s vm=%d execute_ex=%d handler=%d\n",
        $name,
        $result['status'],
        $result['execution']['status'],
        json_encode($result['execution']['return_value']),
        $result['execution']['vm_handler_calls'],
        $result['execution']['execute_ex_calls'],
        $result['execution']['opline_handler_calls'],
    );
}
?>
--EXPECT--
typed_property accepted returned return=[2,"Cannot assign array to reference held by property NativeTypedForeachValue::$value of type int"] vm=0 execute_ex=0 handler=0
readonly_property accepted returned return="Cannot acquire reference to readonly property NativeReadonlyForeachValue::$value" vm=0 execute_ex=0 handler=0
lazy_object accepted returned return=[1,{"value":41}] vm=0 execute_ex=0 handler=0
temporary_object accepted returned return=[2] vm=0 execute_ex=0 handler=0
