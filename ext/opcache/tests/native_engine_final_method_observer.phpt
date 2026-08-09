--TEST--
Native Engine preserves observers for final methods compiled by OPcache
--EXTENSIONS--
opcache
zend_test
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
opcache.optimization_level=-1
zend_test.observer.enabled=1
zend_test.observer.show_output=1
zend_test.observer.observe_function_names=native_engine_observed_final_method,step
zend_test.observer.show_return_value=0
--FILE--
<?php

final class NativeEngineObservedFinalMethod
{
    public function step(int $value): int
    {
        return $value + 1;
    }
}

function native_engine_observed_final_method(int $value): int
{
    $receiver = new NativeEngineObservedFinalMethod();
    return $receiver->step($value);
}

echo native_engine_observed_final_method(41), "\n";
?>
--EXPECTF--
<!-- init '%s' -->
<!-- init native_engine_observed_final_method() -->
<native_engine_observed_final_method>
  <!-- init NativeEngineObservedFinalMethod::step() -->
  <NativeEngineObservedFinalMethod::step>
  </NativeEngineObservedFinalMethod::step>
</native_engine_observed_final_method>
42
