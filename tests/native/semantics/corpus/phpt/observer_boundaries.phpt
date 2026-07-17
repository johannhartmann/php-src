--TEST--
W01 semantic corpus: zend_test forces observer begin, end, and user opcode hook
--EXTENSIONS--
zend_test
--INI--
zend_test.observer.enabled=1
zend_test.observer.show_output=1
zend_test.observer.observe_all=1
zend_test.observer.show_opcode_in_user_handler=ZEND_ECHO
opcache.jit=0
--FILE--
<?php
function observedTarget(): int
{
    echo "body\n";
    return 5;
}

$result = observedTarget();
echo "return:$result\n";
?>
--EXPECTF--
<!-- init '%s' -->
<file '%s'>
  <!-- init observedTarget() -->
  <observedTarget>
    <!-- opcode: 'ZEND_ECHO' in user handler -->
body
  </observedTarget>
  <!-- opcode: 'ZEND_ECHO' in user handler -->
return:5
</file '%s'>
