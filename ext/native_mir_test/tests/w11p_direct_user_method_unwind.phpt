--TEST--
Native baseline unwinds direct monomorphic method frames without retaining receivers
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
final class W11PDirectMethodUnwind
{
    public function fail(): int
    {
        throw new RuntimeException('direct-method');
    }

    public function run(): int
    {
        $receiver = $this;
        try {
            return $receiver->fail();
        } catch (RuntimeException $error) {
            return $error->getMessage() === 'direct-method' ? 42 : 0;
        }
    }
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-direct-user-method-unwind.php',
    [],
    [
        'wave' => 11,
        'function' => 'W11PDirectMethodUnwind::run',
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
accepted return=42 runs=10 codeunits=2 vm=0 execute_ex=0 handler=0 active=0
