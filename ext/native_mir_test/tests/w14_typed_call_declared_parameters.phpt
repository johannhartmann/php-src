--TEST--
Native x64 typed calls keep receive checks of declared boxed parameters
--DESCRIPTION--
A typed call skips the callee's receive opcode. A parameter whose declared
type a boxed zval does not prove (int|float, ?array) therefore takes a typed
call only for arguments proven to satisfy it; others keep coercion and the
TypeError of the Zend call.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function f(int|float $a) { return $a; }
function g($x) { return f($x); }
function h(?array $a) { return $a; }
function k($x) { return h($x); }
var_dump(g(5), g("5"), g(2.5));
try { var_dump(g([])); } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
var_dump(k(null), k([1]));
try { var_dump(k(3)); } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
?>
--EXPECTF--
int(5)
int(5)
float(2.5)
f(): Argument #1 ($a) must be of type int|float, array given, called in %s on line 3
NULL
array(1) {
  [0]=>
  int(1)
}
h(): Argument #1 ($a) must be of type ?array, int given, called in %s on line 5
