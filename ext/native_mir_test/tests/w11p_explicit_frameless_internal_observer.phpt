--TEST--
Native frameless observer calls consume explicit MIR operands
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
zend_test.observer.observe_function_names=w11p_explicit_frameless_observed,strpos
zend_test.observer.show_return_value=0
zend_test.observer.execute_internal=1
--FILE--
<?php
$result = native_mir_test_compile_execute(
    <<<'PHP'
<?php
function w11p_explicit_frameless_observed(
    string $text,
    string $needle,
    int $offset,
): int {
    return strpos($text, $needle, $offset);
}
PHP,
    'w11p-explicit-frameless-observer.php',
    ['alpha-beta', 'beta', 0],
    ['wave' => 11, 'function' => 'w11p_explicit_frameless_observed'],
);
printf(
    "%s return=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
);
?>
--EXPECTF--
<!-- init '%s%ew11p_explicit_frameless_internal_observer.php' -->
<!-- init native_mir_test_compile_execute() -->
<!-- internal enter native_mir_test_compile_execute() -->
  <!-- init w11p_explicit_frameless_observed() -->
  <w11p_explicit_frameless_observed>
    <!-- init strpos() -->
    <strpos>
      <!-- internal enter strpos() -->
      <!-- internal leave strpos() -->
    </strpos>
  </w11p_explicit_frameless_observed>
<!-- internal leave native_mir_test_compile_execute() -->
accepted return=6 vm=0 execute_ex=0 handler=0
