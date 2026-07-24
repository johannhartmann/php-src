--TEST--
Native baseline builds exact typed user frames in generated code
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
function w11p_inline_typed_leaf(
    int $left,
    float $right,
    bool $enabled,
    ?int $optional,
): int {
    return $enabled
        ? $left + (int) $right + ($optional ?? 0)
        : 0;
}

function w11p_inline_typed_root(): int
{
    return w11p_inline_typed_leaf(40, 1.0, true, null) + 1;
}

function w11p_typed_coercion_leaf(int $value): int
{
    return $value;
}

function w11p_typed_coercion_root(): int
{
    return w11p_typed_coercion_leaf('41') + 1;
}
PHP;

foreach (['w11p_inline_typed_root', 'w11p_typed_coercion_root'] as $function) {
    $result = native_mir_test_compile_execute(
        $source,
        'w11p-inline-typed-user-frames.php',
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
w11p_inline_typed_root accepted return=42 runs=10 codeunits=2 vm=0 execute_ex=0 handler=0 active=0
w11p_typed_coercion_root accepted return=42 runs=10 codeunits=2 vm=0 execute_ex=0 handler=0 active=0
