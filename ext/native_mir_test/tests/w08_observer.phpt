--TEST--
Native MIR W08 emits balanced user and internal observer notifications
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
if (!function_exists('zend_trigger_bailout')) {
    die('skip zend_test is not available');
}
?>
--INI--
zend_test.observer.enabled=1
zend_test.observer.show_output=1
zend_test.observer.observe_function_names=native_observed_outer,native_observed_inner,strcmp
zend_test.observer.show_return_value=0
zend_test.observer.execute_internal=1
--FILE--
<?php
$source = <<<'PHP'
<?php
function native_observed_inner(): int
{
    return strcmp('hello', 'hello');
}

function native_observed_outer(): int
{
    return native_observed_inner();
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w08-observer.php',
    [],
    ['wave' => 8, 'function' => 'native_observed_outer'],
);

echo $result['status'], ' ', $result['execution']['status'],
    ' return=', $result['execution']['return_value'],
    ' vm=', $result['execution']['vm_handler_calls'],
    ' execute_ex=', $result['execution']['execute_ex_calls'],
    ' handler=', $result['execution']['opline_handler_calls'], "\n";
?>
--EXPECTF--
<!-- init '%s%ew08_observer.php' -->
<!-- init native_mir_test_compile_execute() -->
<!-- internal enter native_mir_test_compile_execute() -->
  <!-- init native_observed_outer() -->
  <native_observed_outer>
    <!-- init native_observed_inner() -->
    <native_observed_inner>
      <!-- init strcmp() -->
      <strcmp>
        <!-- internal enter strcmp() -->
        <!-- internal leave strcmp() -->
      </strcmp>
    </native_observed_inner>
  </native_observed_outer>
<!-- internal leave native_mir_test_compile_execute() -->
accepted returned return=0 vm=0 execute_ex=0 handler=0
