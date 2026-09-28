--TEST--
Native one-argument frameless calls
--DESCRIPTION--
FRAMELESS_ICALL_1 with a literal, CV or temporary argument calls the handler
like the VM: the result, a dereferenced argument, the release of a temporary.
Undefined CVs and observed functions take the general form.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
declare(strict_types=1);
function a($i) { return [dechex($i), dechex(255), trim("  x "), dirname("/a/b/c")]; }
var_dump(a(10), a(0));
function b() { $s = " pad "; $r = &$s; return [trim($r), trim(str_repeat(" z ", 2)), dechex(PHP_INT_MAX)]; }
var_dump(b());
function c() { return trim($undef ?? "d"); }
var_dump(c());
function d() { return @trim($undef2); }
try { var_dump(d()); } catch (Throwable $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
function e($x) { return dechex($x); }
try { var_dump(e("12")); } catch (Throwable $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
function f($n) { $t = 0; for ($i = 1; $i <= $n; $i++) { $t += strlen(dechex($i)); } return $t; }
var_dump(f(100000));
function g() { return trim($nope); }
var_dump(g());

?>

--EXPECTF--
array(4) {
  [0]=>
  string(1) "a"
  [1]=>
  string(2) "ff"
  [2]=>
  string(1) "x"
  [3]=>
  string(4) "/a/b"
}
array(4) {
  [0]=>
  string(1) "0"
  [1]=>
  string(2) "ff"
  [2]=>
  string(1) "x"
  [3]=>
  string(4) "/a/b"
}
array(3) {
  [0]=>
  string(3) "pad"
  [1]=>
  string(4) "z  z"
  [2]=>
  string(16) "7fffffffffffffff"
}
string(1) "d"
TypeError: trim(): Argument #1 ($string) must be of type string, null given
TypeError: dechex(): Argument #1 ($num) must be of type int, string given
int(430100)

Warning: Undefined variable $nope in %s on line 15

Fatal error: Uncaught TypeError: trim(): Argument #1 ($string) must be of type string, null given in %s:15
Stack trace:
#0 %s(16): g()
#1 {main}
  thrown in %s on line 15
