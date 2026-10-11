--TEST--
Native compound assignment to a property warns for an undefined right-hand side
--DESCRIPTION--
The OP_DATA of ASSIGN_OBJ_OP and ASSIGN_STATIC_PROP_OP reads as BP_VAR_R: an undefined CV warns, in the VM's order, and counts as null.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class C { public $p = 1; public string $s = 'x'; public static $sp = 2; public static int $si = 3; }
function f() {
    $o = new C;
    $o->p += $undef1;
    $o->s .= $undef2;
    var_dump($o->p, $o->s);
    C::$sp .= $undef3;
    C::$si += $undef4;
    var_dump(C::$sp, C::$si);
    $n = null;
    try { $n->p .= $undef5; } catch (Error $e) { echo $e->getMessage(), "\n"; }
    try { $undefobj->p .= $undef6; } catch (Error $e) { echo $e->getMessage(), "\n"; }
    try { C::$nope .= $undef7; } catch (Error $e) { echo $e->getMessage(), "\n"; }
    $name = 'p';
    $o->$undefname .= $undef8;
    var_dump($o);
    $a = [];
    $a['k'] .= $undef9;
    var_dump($a);
    $o->p = 'q';
    var_dump($o->p .= $undef10);
}
f();
set_error_handler(function ($no, $str) { throw new Exception($str); });
function g() {
    $o = new C;
    try { $o->p += $undef1; } catch (Exception $e) { echo "caught ", $e->getMessage(), "\n"; }
    var_dump($o->p);
    try { C::$sp .= $undef2; } catch (Exception $e) { echo "caught ", $e->getMessage(), "\n"; }
    var_dump(C::$sp);
}
g();
--EXPECTF--

Warning: Undefined variable $undef1 in %s on line 5

Warning: Undefined variable $undef2 in %s on line 6
int(1)
string(1) "x"

Warning: Undefined variable $undef3 in %s on line 8

Warning: Undefined variable $undef4 in %s on line 9
string(1) "2"
int(3)

Warning: Undefined variable $undef5 in %s on line 12
Attempt to assign property "p" on null

Warning: Undefined variable $undef6 in %s on line 13

Warning: Undefined variable $undefobj in %s on line 13
Attempt to assign property "p" on null
Access to undeclared static property C::$nope

Warning: Undefined variable $undefname in %s on line 16

Warning: Undefined variable $undef8 in %s on line 16

Deprecated: Creation of dynamic property C::$ is deprecated in %s on line 16

Warning: Undefined property: C::$ in %s on line 16
object(C)#1 (3) {
  ["p"]=>
  int(1)
  ["s"]=>
  string(1) "x"
  [""]=>
  string(0) ""
}

Warning: Undefined array key "k" in %s on line 19

Warning: Undefined variable $undef9 in %s on line 19
array(1) {
  ["k"]=>
  string(0) ""
}

Warning: Undefined variable $undef10 in %s on line 22
string(1) "q"
caught Undefined variable $undef1
int(1)
caught Undefined variable $undef2
string(1) "2"
