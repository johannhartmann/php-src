--TEST--
An undefined CV assigned to a static property warns like the VM; by reference it does not
--FILE--
<?php
class C { public static $p = 1; public static ?int $t = 1; }
function f() { C::$p = $undef; var_dump(C::$p); }
function g() { C::$t = $undef2; var_dump(C::$t); }
for ($i = 0; $i < 3; $i++) { f(); g(); }
class D { public static int $i = 1; public static $r; }
function h() { try { D::$i = $u3; } catch (TypeError $e) { echo $e->getMessage(), "\n"; } var_dump(D::$i); }
function k() { D::$r = &$u4; var_dump(D::$r, $u4); $x = C::$p = $u5; var_dump($x); }
set_error_handler(function ($n, $m) { throw new Exception($m); });
function t() { try { C::$p = $u6; } catch (Exception $e) { echo "caught ", $e->getMessage(), "\n"; } var_dump(C::$p); }
for ($i = 0; $i < 2; $i++) { restore_error_handler(); h(); k(); set_error_handler(function ($n, $m) { throw new Exception($m); }); t(); }
?>
--EXPECTF--

Warning: Undefined variable $undef in %s on line 3
NULL

Warning: Undefined variable $undef2 in %s on line 4
NULL

Warning: Undefined variable $undef in %s on line 3
NULL

Warning: Undefined variable $undef2 in %s on line 4
NULL

Warning: Undefined variable $undef in %s on line 3
NULL

Warning: Undefined variable $undef2 in %s on line 4
NULL

Warning: Undefined variable $u3 in %s on line 7
Cannot assign null to property D::$i of type int
int(1)
NULL
NULL

Warning: Undefined variable $u5 in %s on line 8
NULL
caught Undefined variable $u6
NULL

Warning: Undefined variable $u3 in %s on line 7
Cannot assign null to property D::$i of type int
int(1)
NULL
NULL

Warning: Undefined variable $u5 in %s on line 8
NULL
caught Undefined variable $u6
NULL
