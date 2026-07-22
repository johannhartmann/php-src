--TEST--
Native MIR W08 executes normal, exceptional, and early-return finally paths
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$cases = [
    'normal_finally' => <<<'PHP'
<?php
function normal_finally(): int
{
    try {
        intdiv(8, 2);
    } finally {
        intdiv(6, 3);
    }
    return 4;
}
PHP,
    'caught_after_finally' => <<<'PHP'
<?php
function caught_after_finally(): int
{
    try {
        try {
            intdiv(1, 0);
        } finally {
            intdiv(6, 3);
        }
    } catch (DivisionByZeroError) {
        return 7;
    }
    return 0;
}
PHP,
    'early_return_finally' => <<<'PHP'
<?php
function early_return_finally(bool $take): int
{
    try {
        if ($take) {
            return 2;
        }
    } finally {
        intdiv(6, 3);
    }
    return 3;
}
PHP,
    'exception_replaced_in_finally' => <<<'PHP'
<?php
function exception_replaced_in_finally(): int
{
    try {
        try {
            intdiv(1, 0);
        } finally {
            range(1, 2, 0);
        }
    } catch (ValueError) {
        return 9;
    } catch (DivisionByZeroError) {
        return -1;
    }
    return 0;
}
PHP,
];

foreach ($cases as $function => $source) {
    $args = $function === 'early_return_finally' ? [true] : [];
    $result = native_mir_test_compile_execute(
        $source,
        "w08-$function.php",
        $args,
        ['wave' => 8, 'function' => $function],
    );
    printf(
        "%s %s %s return=%s exception=%d bailout=%d vm=%d execute_ex=%d handler=%d\n",
        $function,
        $result['status'],
        $result['execution']['status'] ?? '-',
        json_encode($result['execution']['return_value'] ?? null),
        $result['execution']['exception'] ?? -1,
        $result['execution']['bailout'] ?? -1,
        $result['execution']['vm_handler_calls'] ?? -1,
        $result['execution']['execute_ex_calls'] ?? -1,
        $result['execution']['opline_handler_calls'] ?? -1,
    );
}
?>
--EXPECT--
normal_finally accepted returned return=4 exception=0 bailout=0 vm=0 execute_ex=0 handler=0
caught_after_finally accepted returned return=7 exception=0 bailout=0 vm=0 execute_ex=0 handler=0
early_return_finally accepted returned return=2 exception=0 bailout=0 vm=0 execute_ex=0 handler=0
exception_replaced_in_finally accepted returned return=9 exception=0 bailout=0 vm=0 execute_ex=0 handler=0
