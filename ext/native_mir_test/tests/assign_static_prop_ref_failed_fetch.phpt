--TEST--
A failed static property fetch leaves an undefined OP_DATA CV of a reference assignment undefined
--FILE--
<?php
class A { public static $q; }
function f() {
    try { Missing::$p = &$u; } catch (Error $e) { echo $e->getMessage(), "\n"; }
    var_dump($u);
    try { A::$nope = &$w; } catch (Error $e) { echo $e->getMessage(), "\n"; }
    var_dump($w);
    A::$q = &$v;
    var_dump($v);
}
f();
f();
?>
--EXPECTF--
Class "Missing" not found

Warning: Undefined variable $u in %s on line 5
NULL
Access to undeclared static property A::$nope

Warning: Undefined variable $w in %s on line 7
NULL
NULL
Class "Missing" not found

Warning: Undefined variable $u in %s on line 5
NULL
Access to undeclared static property A::$nope

Warning: Undefined variable $w in %s on line 7
NULL
NULL
