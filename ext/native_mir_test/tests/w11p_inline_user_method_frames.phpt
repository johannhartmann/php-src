--TEST--
Native baseline builds instance and fixed-scope static method frames in generated code
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
final class W11PInlineInstanceMethod
{
    public function leaf($value)
    {
        return $value;
    }

    public function root()
    {
        return $this->leaf(41) + 1;
    }
}

final class W11PInlineStaticMethod
{
    public static function leaf($value)
    {
        return $value;
    }
}

function w11p_inline_static_root()
{
    return W11PInlineStaticMethod::leaf(41) + 1;
}
PHP;

foreach (['W11PInlineInstanceMethod::root', 'w11p_inline_static_root'] as $function) {
    $result = native_mir_test_compile_execute(
        $source,
        'w11p-inline-user-method-frames.php',
        [],
        [
            'wave' => 11,
            'function' => $function,
            'repeat' => 10,
        ],
    );
    printf(
        "%s %s return=%d runs=%d codeunits=%d vm=%d execute_ex=%d handler=%d active=%d\n",
        $function,
        $result['status'],
        $result['execution']['return_value'],
        $result['execution']['executions'],
        $result['execution']['native_codeunits'],
        $result['execution']['vm_handler_calls'],
        $result['execution']['execute_ex_calls'],
        $result['execution']['opline_handler_calls'],
        $result['execution']['entry_active_calls'],
    );
}
?>
--EXPECT--
W11PInlineInstanceMethod::root accepted return=42 runs=10 codeunits=2 vm=0 execute_ex=0 handler=0 active=0
w11p_inline_static_root accepted return=42 runs=10 codeunits=2 vm=0 execute_ex=0 handler=0 active=0
