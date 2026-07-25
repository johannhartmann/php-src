--TEST--
Native MIR W12 executes specialized baseline value opcodes without VM dispatch
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
final class W12Countable implements Countable
{
    public function count(): int
    {
        return 4;
    }
}
class W12Scope
{
    public function instance(): bool
    {
        return isset($this);
    }

    public static function called(): string
    {
        return get_called_class();
    }
}
function w12_baseline_values(mixed $value): array
{
    $reporting = error_reporting();
    $silent = @strlen("native");
    try {
        @strlen([]);
    } catch (TypeError) {
        $restoredAfterException = error_reporting() === $reporting;
    }
    try {
        match (9) {
            1 => 2,
        };
    } catch (UnhandledMatchError) {
        $match = "caught";
    }
    return [
        count([1, 2]),
        count(new W12Countable()),
        gettype($value),
        array_key_exists(1, [1 => null]),
        in_array("2", [1, 2], false),
        in_array("2", [1, 2], true),
        $silent,
        error_reporting() === $reporting,
        $restoredAfterException,
        $match,
    ];
}
PHP;

$cases = [
    ['w12_baseline_values', [[]]],
    ['W12Scope::instance', []],
    ['W12Scope::called', []],
];
foreach ($cases as [$function, $arguments]) {
    $result = native_mir_test_compile_execute(
        $source,
        'w12-baseline-values.php',
        $arguments,
        ['wave' => 11, 'function' => $function],
    );
    printf(
        "%s status=%s result=%s vm=%d execute_ex=%d handler=%d active=%d\n",
        $function,
        $result['status'],
        json_encode($result['execution']['return_value'] ?? null),
        $result['execution']['vm_handler_calls'] ?? -1,
        $result['execution']['execute_ex_calls'] ?? -1,
        $result['execution']['opline_handler_calls'] ?? -1,
        $result['execution']['entry_active_calls'] ?? -1,
    );
}
?>
--EXPECT--
w12_baseline_values status=accepted result=[2,4,"array",true,true,false,6,true,true,"caught"] vm=0 execute_ex=0 handler=0 active=0
W12Scope::instance status=accepted result=true vm=0 execute_ex=0 handler=0 active=0
W12Scope::called status=accepted result="W12Scope" vm=0 execute_ex=0 handler=0 active=0
