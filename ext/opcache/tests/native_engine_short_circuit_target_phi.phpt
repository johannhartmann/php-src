--TEST--
Native optimized short-circuit target preserves its general-purpose PHI result
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.optimization_level=-1
opcache.file_update_protection=0
--EXTENSIONS--
opcache
--FILE--
<?php
function native_short_circuit_target_phi(): array
{
    $value = [];
    $result = false
        || ((is_array($value) || $value instanceof Countable) && true)
        || false;

    return [$result, $result ? 'taken' : 'missed'];
}

var_dump(native_short_circuit_target_phi());
?>
--EXPECT--
array(2) {
  [0]=>
  bool(true)
  [1]=>
  string(5) "taken"
}
