--TEST--
Native baseline enters monomorphic user methods through direct native frames
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
final class W11PDirectMethod
{
    private int $calls = 0;

    public function leaf(int $value): int
    {
        return $value + ++$this->calls;
    }

    public function run(): int
    {
        $this->calls = 0;
        $other = $this;
        return $this->leaf(20) + $other->leaf(20);
    }
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-direct-user-methods.php',
    [],
    [
        'wave' => 11,
        'function' => 'W11PDirectMethod::run',
        'repeat' => 10,
    ],
);
printf(
    "%s return=%d runs=%d codeunits=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['executions'],
    $result['execution']['native_codeunits'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=43 runs=10 codeunits=2 vm=0 execute_ex=0 handler=0 active=0
