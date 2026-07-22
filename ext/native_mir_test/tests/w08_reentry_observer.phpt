--TEST--
Native MIR W08 emits one balanced observer pair for native callback reentry
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
if (!extension_loaded('zend_test')) {
    die('skip zend_test is not available');
}
?>
--INI--
zend_test.observer.enabled=1
zend_test.observer.show_output=1
zend_test.observer.observe_function_names=native_reentry_observed,native_callback_observed,array_map
zend_test.observer.show_return_value=0
zend_test.observer.execute_internal=1
--FILE--
<?php
$result = native_mir_test_compile_execute(
    <<<'PHP'
<?php
function native_callback_observed(int $value): int
{
    return $value;
}

function native_reentry_observed(): int
{
    array_map('native_callback_observed', [4]);
    return 7;
}
PHP,
    'w08-reentry-observer.php',
    [],
    ['wave' => 8, 'function' => 'native_reentry_observed'],
);
printf(
    "%s %s return=%s vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $result['execution']['status'],
    $result['execution']['return_value'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
);
?>
--EXPECTF--
<!-- init '%s%ew08_reentry_observer.php' -->
<!-- init native_mir_test_compile_execute() -->
<!-- internal enter native_mir_test_compile_execute() -->
  <!-- init native_reentry_observed() -->
  <native_reentry_observed>
    <!-- init array_map() -->
    <array_map>
      <!-- internal enter array_map() -->
        <!-- init native_callback_observed() -->
        <native_callback_observed>
        </native_callback_observed>
      <!-- internal leave array_map() -->
    </array_map>
  </native_reentry_observed>
<!-- internal leave native_mir_test_compile_execute() -->
accepted returned return=7 vm=0 execute_ex=0 handler=0
