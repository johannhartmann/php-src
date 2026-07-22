--TEST--
Native MIR W08 routes internal-call user callbacks through native entry cells
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
function native_callback(int $value): int
{
    echo $value;
    return $value;
}

function native_reentry(): int
{
    array_map('native_callback', [4]);
    return 7;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w08-reentry.php',
    [],
    ['wave' => 8, 'function' => 'native_reentry', 'stack_probe' => true],
);
$frame = $result['execution']['stack_trace'][0] ?? [];
printf(
    "\n%s %s return=%s frame=%d probes=%d caller=%s callee=%s "
        . "vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $result['execution']['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['frame_chain_valid'],
    count($result['execution']['stack_trace']),
    $frame['caller'] ?? '-',
    $frame['callee'] ?? '-',
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
);

$caught = native_mir_test_compile_execute(
    <<<'PHP'
<?php
function throwing_callback(int $value): int
{
    return intdiv($value, 0);
}

function catch_callback_exception(): int
{
    try {
        array_map('throwing_callback', [4]);
    } catch (DivisionByZeroError) {
        return 9;
    }
    return 0;
}
PHP,
    'w08-reentry-exception.php',
    [],
    ['wave' => 8, 'function' => 'catch_callback_exception'],
);
printf(
    "%s %s return=%s exception=%d vm=%d execute_ex=%d handler=%d\n",
    $caught['status'],
    $caught['execution']['status'],
    json_encode($caught['execution']['return_value']),
    $caught['execution']['exception'],
    $caught['execution']['vm_handler_calls'],
    $caught['execution']['execute_ex_calls'],
    $caught['execution']['opline_handler_calls'],
);

$dynamic = native_mir_test_compile_execute(
    <<<'PHP'
<?php
function unresolved_reentry(): int
{
    array_map('missing_callback', [4]);
    return 7;
}
PHP,
    'w08-dynamic-reentry.php',
    [],
    ['wave' => 8, 'function' => 'unresolved_reentry'],
);
$dynamicDiagnostic = end($dynamic['diagnostics']);
printf(
    "%s %s %s machine=%d\n",
    $dynamic['status'],
    $dynamic['phase'],
    $dynamicDiagnostic['code'],
    $dynamic['execution']['machine_code'] !== null,
);
?>
--EXPECT--
4
accepted returned return=7 frame=1 probes=1 caller=array_map callee=native_callback vm=0 execute_ex=0 handler=0
accepted returned return=9 exception=0 vm=0 execute_ex=0 handler=0
rejected codegen NATIVE0003 machine=0
