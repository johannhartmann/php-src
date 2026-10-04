--TEST--
Native executes static-init and frameless lookup branches directly
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$staticSource = <<<'PHP'
<?php
function native_runtime_branch_seed(): int
{
    return 40;
}
function native_runtime_branch_static(): int
{
    static $value = native_runtime_branch_seed();
    return ++$value;
}
PHP;

$static = native_mir_test_compile_execute(
    $staticSource,
    'runtime-static.php',
    [],
    [
        'function' => 'native_runtime_branch_static',
        'repeat' => 2,
    ],
);
printf(
    "static status=%s opcode=%s return=%s vm=%d execute_ex=%d handler=%d\n",
    $static['status'],
    in_array(
        'ZEND_BIND_INIT_STATIC_OR_JMP',
        $static['source_opcodes'],
        true,
    ) ? 'ZEND_BIND_INIT_STATIC_OR_JMP' : 'missing',
    json_encode($static['execution']['return_value'] ?? null),
    $static['execution']['vm_handler_calls'] ?? -1,
    $static['execution']['execute_ex_calls'] ?? -1,
    $static['execution']['opline_handler_calls'] ?? -1,
);

$hitSource = <<<'PHP'
<?php
namespace NativeFramelessHit;
function target(string $value): string
{
    return trim($value);
}
PHP;

$hit = native_mir_test_compile_execute(
    $hitSource,
    'runtime-frameless-hit.php',
    ['  native  '],
    ['function' => 'NativeFramelessHit\\target'],
);
printf(
    "frameless-hit status=%s opcode=%s return=%s vm=%d execute_ex=%d handler=%d\n",
    $hit['status'],
    in_array('ZEND_JMP_FRAMELESS', $hit['source_opcodes'], true)
        ? 'ZEND_JMP_FRAMELESS' : 'missing',
    json_encode($hit['execution']['return_value'] ?? null),
    $hit['execution']['vm_handler_calls'] ?? -1,
    $hit['execution']['execute_ex_calls'] ?? -1,
    $hit['execution']['opline_handler_calls'] ?? -1,
);

$missSource = <<<'PHP'
<?php
namespace NativeFramelessMiss;
function target(string $value): string
{
    eval(
        'namespace NativeFramelessMiss; '
        . 'function trim(string $value): string { return "local:" . $value; }'
    );
    return trim($value);
}
PHP;

$miss = native_mir_test_compile_execute(
    $missSource,
    'runtime-frameless-miss.php',
    ['native'],
    ['function' => 'NativeFramelessMiss\\target'],
);
printf(
    "frameless-miss status=%s opcode=%s return=%s vm=%d execute_ex=%d handler=%d\n",
    $miss['status'],
    in_array('ZEND_JMP_FRAMELESS', $miss['source_opcodes'], true)
        ? 'ZEND_JMP_FRAMELESS' : 'missing',
    json_encode($miss['execution']['return_value'] ?? null),
    $miss['execution']['vm_handler_calls'] ?? -1,
    $miss['execution']['execute_ex_calls'] ?? -1,
    $miss['execution']['opline_handler_calls'] ?? -1,
);
?>
--EXPECT--
static status=accepted opcode=ZEND_BIND_INIT_STATIC_OR_JMP return=42 vm=0 execute_ex=0 handler=0
frameless-hit status=accepted opcode=ZEND_JMP_FRAMELESS return="native" vm=0 execute_ex=0 handler=0
frameless-miss status=accepted opcode=ZEND_JMP_FRAMELESS return="local:native" vm=0 execute_ex=0 handler=0
