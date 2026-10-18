--TEST--
Native: arguments of a declared false, true or null type return their value (unoptimized)
--DESCRIPTION--
A scalar the ABI keeps boxed returns both parts of its zval, not the type
of its exact scalar fact.
--INI--
opcache.enable_cli=0
--FILE--
<?php
function f_false(false $v) { var_dump($v); return $v; }
function f_true(true $v) { return $v; }
function f_null(null $v) { return $v; }
function r_false(false $v): false { return $v; }
function r_true(true $v): true { return $v; }
function r_union(false $v): false|int { return $v; }
function r_bool(bool $v): bool { return $v; }
var_dump(f_false(false), f_true(true), f_null(null), r_false(false), r_true(true), r_union(false), r_bool(false), r_bool(true));
$f = 'f_false'; var_dump($f(false));
try { f_false(true); } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
try { r_true(false); } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
?>
--EXPECTF--
bool(false)
bool(false)
bool(true)
NULL
bool(false)
bool(true)
bool(false)
bool(false)
bool(true)
bool(false)
bool(false)
f_false(): Argument #1 ($v) must be of type false, true given, called in %s on line 11
r_true(): Argument #1 ($v) must be of type true, false given, called in %s on line 12
