--TEST--
Native helpers consuming direct call results
--DESCRIPTION--
Throw, echo, print, static assignment, array literals, clone, isset and concatenation of direct call results behave like the VM.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function g($x) { return $x; }
function gi(int $x): int { return $x * 2; }
function mkex($m) { return new Exception($m); }
function t($m) { throw mkex($m); }
function y($v) { yield g($v); yield gi($v); }
function ech($v) { echo g($v), gi($v), "\n"; }
function prn($v) { print g($v); return gi($v); }
function st($v) { static $s; $s = g($v); return $s; }
function arr($v) { return [g($v), gi($v)]; }
function cl($v) { return clone g($v); }
function inc($v) { include_once g($v); }
function isn($v) { return isset($v[g(0)]); }
function str($v) { return "a" . g($v) . gi($v); }
try { t("boom"); } catch (Exception $e) { echo $e->getMessage(), "\n"; }
ech(4); var_dump(prn(5), st(6), arr(7), cl(new ArrayObject([1])), isn([8]), str(9));
?>
--EXPECT--
boom
48
5int(10)
int(6)
array(2) {
  [0]=>
  int(7)
  [1]=>
  int(14)
}
object(ArrayObject)#3 (1) {
  ["storage":"ArrayObject":private]=>
  array(1) {
    [0]=>
    int(1)
  }
}
bool(true)
string(4) "a918"
