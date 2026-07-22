--TEST--
Native MIR W08 validates every runtime helper before image publication
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
function native_runtime_contract(): int
{
    return 42;
}
PHP;

for ($helper = 1; $helper <= 20; $helper++) {
    $result = native_mir_test_compile_execute(
        $source,
        "w08-runtime-helper-$helper.php",
        [],
        [
            'wave' => 8,
            'function' => 'native_runtime_contract',
            'runtime_helper_failure' => $helper,
        ],
    );
    $diagnostic = end($result['diagnostics']);
    printf(
        "%02d=%s phase=%s code=%s machine=%d executions=%d vm=%d execute_ex=%d handler=%d\n",
        $helper,
        $result['status'],
        $result['phase'],
        $diagnostic['code'],
        $result['execution']['machine_code'] !== null,
        $result['execution']['executions'],
        $result['execution']['vm_handler_calls'],
        $result['execution']['execute_ex_calls'],
        $result['execution']['opline_handler_calls'],
    );
}
?>
--EXPECTF--
01=error phase=codegen code=NATIVE0001 machine=0 executions=0 vm=0 execute_ex=0 handler=0
02=error phase=codegen code=NATIVE0001 machine=0 executions=0 vm=0 execute_ex=0 handler=0
03=error phase=codegen code=NATIVE0001 machine=0 executions=0 vm=0 execute_ex=0 handler=0
04=error phase=codegen code=NATIVE0001 machine=0 executions=0 vm=0 execute_ex=0 handler=0
05=error phase=codegen code=NATIVE0001 machine=0 executions=0 vm=0 execute_ex=0 handler=0
06=error phase=codegen code=NATIVE0001 machine=0 executions=0 vm=0 execute_ex=0 handler=0
07=error phase=codegen code=NATIVE0001 machine=0 executions=0 vm=0 execute_ex=0 handler=0
08=error phase=codegen code=NATIVE0001 machine=0 executions=0 vm=0 execute_ex=0 handler=0
09=error phase=codegen code=NATIVE0001 machine=0 executions=0 vm=0 execute_ex=0 handler=0
10=error phase=codegen code=NATIVE0001 machine=0 executions=0 vm=0 execute_ex=0 handler=0
11=error phase=codegen code=NATIVE0001 machine=0 executions=0 vm=0 execute_ex=0 handler=0
12=error phase=codegen code=NATIVE0001 machine=0 executions=0 vm=0 execute_ex=0 handler=0
13=error phase=codegen code=NATIVE0001 machine=0 executions=0 vm=0 execute_ex=0 handler=0
14=error phase=codegen code=NATIVE0001 machine=0 executions=0 vm=0 execute_ex=0 handler=0
15=error phase=codegen code=NATIVE0001 machine=0 executions=0 vm=0 execute_ex=0 handler=0
16=error phase=codegen code=NATIVE0001 machine=0 executions=0 vm=0 execute_ex=0 handler=0
17=error phase=codegen code=NATIVE0001 machine=0 executions=0 vm=0 execute_ex=0 handler=0
18=error phase=codegen code=NATIVE0001 machine=0 executions=0 vm=0 execute_ex=0 handler=0
19=error phase=codegen code=NATIVE0001 machine=0 executions=0 vm=0 execute_ex=0 handler=0
20=error phase=codegen code=NATIVE0001 machine=0 executions=0 vm=0 execute_ex=0 handler=0
