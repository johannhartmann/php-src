--TEST--
Native missing array reads materialize null
--SKIPIF--
<?php
if (!extension_loaded('native_mir_test')) {
    die('skip native_mir_test extension not available');
}
?>
--FILE--
<?php
function nativeMissingArrayRead(array $values): mixed
{
    $value = $values['missing'];
    var_dump($value);
    return $value;
}

var_dump(nativeMissingArrayRead([]));
?>
--EXPECTF--
Warning: Undefined array key "missing" in %s on line %d
NULL
NULL
