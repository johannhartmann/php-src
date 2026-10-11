--TEST--
Native call paths warn about undefined variables through the shared cold helper
--DESCRIPTION--
Frameless calls, fast sends, method, dynamic, static and new targets read
undefined CVs as the VM's zval_undefined_cv() does: the warning (also
through an error handler that throws), then null.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function take($a, $b = 'd') { return gettype($a) . $b; }
function frameless_sites() {
    $r = [];
    $r[] = in_array($u1, [null]);
    $r[] = str_contains('abc', $u2 ?? 'b');
    $r[] = min($u3, 4);
    $r[] = implode(',', [$u4]);
    $r[] = str_replace('a', 'b', $u5);
    return $r;
}
function send_sites() {
    $r = [];
    for ($i = 0; $i < 2; $i++) {
        $r[] = take($m1);
        $r[] = take(1, $m2);
    }
    return $r;
}
function method_sites() {
    try { $o->m(); } catch (Error $e) { echo $e->getMessage(), "\n"; }
    try { $f(); } catch (Error $e) { echo $e->getMessage(), "\n"; }
    try { $cls::m(); } catch (Error $e) { echo $e->getMessage(), "\n"; }
    try { new $k; } catch (Error $e) { echo $e->getMessage(), "\n"; }
}
set_error_handler(function ($no, $msg) { echo "handler: $msg\n"; return true; });
var_dump(frameless_sites());
var_dump(send_sites());
method_sites();
restore_error_handler();
function throwing_handler() {
    set_error_handler(function ($no, $msg) { throw new Exception($msg); });
    try { in_array($z, []); } catch (Exception $e) { echo "caught ", $e->getMessage(), "\n"; }
    try { take($y); } catch (Exception $e) { echo "caught ", $e->getMessage(), "\n"; }
    restore_error_handler();
}
throwing_handler();
var_dump(frameless_sites());
?>
--EXPECTF--
handler: Undefined variable $u1
handler: Undefined variable $u3
handler: Undefined variable $u4
handler: Undefined variable $u5
handler: str_replace(): Passing null to parameter #3 ($subject) of type array|string is deprecated
array(5) {
  [0]=>
  bool(true)
  [1]=>
  bool(true)
  [2]=>
  NULL
  [3]=>
  string(0) ""
  [4]=>
  string(0) ""
}
handler: Undefined variable $m1
handler: Undefined variable $m2
handler: Undefined variable $m1
handler: Undefined variable $m2
array(4) {
  [0]=>
  string(5) "NULLd"
  [1]=>
  string(7) "integer"
  [2]=>
  string(5) "NULLd"
  [3]=>
  string(7) "integer"
}
handler: Undefined variable $o
Call to a member function m() on null
handler: Undefined variable $f
Value of type null is not callable
handler: Undefined variable $cls
Class name must be a valid object or a string
handler: Undefined variable $k
Class name must be a valid object or a string
caught Undefined variable $z
caught Undefined variable $y

Warning: Undefined variable $u1 in %s on line 5

Warning: Undefined variable $u3 in %s on line 7

Warning: Undefined variable $u4 in %s on line 8

Warning: Undefined variable $u5 in %s on line 9

Deprecated: str_replace(): Passing null to parameter #3 ($subject) of type array|string is deprecated in %s on line 9
array(5) {
  [0]=>
  bool(true)
  [1]=>
  bool(true)
  [2]=>
  NULL
  [3]=>
  string(0) ""
  [4]=>
  string(0) ""
}
