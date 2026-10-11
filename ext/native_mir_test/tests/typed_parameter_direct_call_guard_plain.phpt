--TEST--
Native direct calls check typed parameters of untyped arguments (without opcache)
--DESCRIPTION--
A direct user call whose argument's type only a run-time guard proves keeps the guarded inline frame: a mismatch takes the canonical call, whose RECV coerces or throws exactly like the VM, for read and unread parameters, dynamic calls, call results and methods.
--INI--
opcache.enable=0
opcache.enable_cli=0
--FILE--
<?php
function second(int $a, int $b): int { return $b + 1; }
function both(int $a, int $b): int { return $a + $b; }
function fl(float $a): float { return $a * 2; }
function bo(bool $a): int { return $a ? 1 : 0; }
function st(string $a): int { return strlen($a); }
function un(int $a, $b) { return $b; }
class K { public function m(int $a): int { return $a + 1; } public static function s(int $a, int $b): int { return $b; } }
function uc($v) { return second($v, 1); }
function ub($v) { return both($v, 1); }
function ufl($v) { return fl($v); }
function ubo($v) { return bo($v); }
function ust($v) { return st($v); }
function uun($v) { return un($v, 7); }
function udyn($v) { $f = 'second'; return $f($v, 1); }
function ures($v) { return second(strtolower($v), 1); }
function ucat($v) { return both($v . "", 2); }
function umeth($v) { return (new K)->m($v); }
function ustat($v) { return K::s($v, 3); }
function uloop($v) { $s = 0; for ($i = 0; $i < 3; $i++) { $s += both($v, $i); } return $s; }
$values = ["x", "5", "5.5", 5.0, 5.5, true, false, null, 7, [1], "12abc", " 3"];
foreach (['uc', 'ub', 'ufl', 'ubo', 'ust', 'uun', 'udyn', 'ures', 'ucat', 'umeth', 'ustat', 'uloop'] as $f) {
    foreach ($values as $v) {
        echo $f, "(", json_encode($v), "): ";
        try { var_dump($f($v)); } catch (TypeError $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
    }
}
?>
--EXPECTF--
uc("x"): TypeError: second(): Argument #1 ($a) must be of type int, string given, called in %s on line 9
uc("5"): int(2)
uc("5.5"): 
Deprecated: Implicit conversion from float-string "5.5" to int loses precision in %s on line 2
int(2)
uc(5): int(2)
uc(5.5): 
Deprecated: Implicit conversion from float 5.5 to int loses precision in %s on line 2
int(2)
uc(true): int(2)
uc(false): int(2)
uc(null): TypeError: second(): Argument #1 ($a) must be of type int, null given, called in %s on line 9
uc(7): int(2)
uc([1]): TypeError: second(): Argument #1 ($a) must be of type int, array given, called in %s on line 9
uc("12abc"): TypeError: second(): Argument #1 ($a) must be of type int, string given, called in %s on line 9
uc(" 3"): int(2)
ub("x"): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 10
ub("5"): int(6)
ub("5.5"): 
Deprecated: Implicit conversion from float-string "5.5" to int loses precision in %s on line 3
int(6)
ub(5): int(6)
ub(5.5): 
Deprecated: Implicit conversion from float 5.5 to int loses precision in %s on line 3
int(6)
ub(true): int(2)
ub(false): int(1)
ub(null): TypeError: both(): Argument #1 ($a) must be of type int, null given, called in %s on line 10
ub(7): int(8)
ub([1]): TypeError: both(): Argument #1 ($a) must be of type int, array given, called in %s on line 10
ub("12abc"): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 10
ub(" 3"): int(4)
ufl("x"): TypeError: fl(): Argument #1 ($a) must be of type float, string given, called in %s on line 11
ufl("5"): float(10)
ufl("5.5"): float(11)
ufl(5): float(10)
ufl(5.5): float(11)
ufl(true): float(2)
ufl(false): float(0)
ufl(null): TypeError: fl(): Argument #1 ($a) must be of type float, null given, called in %s on line 11
ufl(7): float(14)
ufl([1]): TypeError: fl(): Argument #1 ($a) must be of type float, array given, called in %s on line 11
ufl("12abc"): TypeError: fl(): Argument #1 ($a) must be of type float, string given, called in %s on line 11
ufl(" 3"): float(6)
ubo("x"): int(1)
ubo("5"): int(1)
ubo("5.5"): int(1)
ubo(5): int(1)
ubo(5.5): int(1)
ubo(true): int(1)
ubo(false): int(0)
ubo(null): TypeError: bo(): Argument #1 ($a) must be of type bool, null given, called in %s on line 12
ubo(7): int(1)
ubo([1]): TypeError: bo(): Argument #1 ($a) must be of type bool, array given, called in %s on line 12
ubo("12abc"): int(1)
ubo(" 3"): int(1)
ust("x"): int(1)
ust("5"): int(1)
ust("5.5"): int(3)
ust(5): int(1)
ust(5.5): int(3)
ust(true): int(1)
ust(false): int(0)
ust(null): TypeError: st(): Argument #1 ($a) must be of type string, null given, called in %s on line 13
ust(7): int(1)
ust([1]): TypeError: st(): Argument #1 ($a) must be of type string, array given, called in %s on line 13
ust("12abc"): int(5)
ust(" 3"): int(2)
uun("x"): TypeError: un(): Argument #1 ($a) must be of type int, string given, called in %s on line 14
uun("5"): int(7)
uun("5.5"): 
Deprecated: Implicit conversion from float-string "5.5" to int loses precision in %s on line 7
int(7)
uun(5): int(7)
uun(5.5): 
Deprecated: Implicit conversion from float 5.5 to int loses precision in %s on line 7
int(7)
uun(true): int(7)
uun(false): int(7)
uun(null): TypeError: un(): Argument #1 ($a) must be of type int, null given, called in %s on line 14
uun(7): int(7)
uun([1]): TypeError: un(): Argument #1 ($a) must be of type int, array given, called in %s on line 14
uun("12abc"): TypeError: un(): Argument #1 ($a) must be of type int, string given, called in %s on line 14
uun(" 3"): int(7)
udyn("x"): TypeError: second(): Argument #1 ($a) must be of type int, string given, called in %s on line 15
udyn("5"): int(2)
udyn("5.5"): 
Deprecated: Implicit conversion from float-string "5.5" to int loses precision in %s on line 2
int(2)
udyn(5): int(2)
udyn(5.5): 
Deprecated: Implicit conversion from float 5.5 to int loses precision in %s on line 2
int(2)
udyn(true): int(2)
udyn(false): int(2)
udyn(null): TypeError: second(): Argument #1 ($a) must be of type int, null given, called in %s on line 15
udyn(7): int(2)
udyn([1]): TypeError: second(): Argument #1 ($a) must be of type int, array given, called in %s on line 15
udyn("12abc"): TypeError: second(): Argument #1 ($a) must be of type int, string given, called in %s on line 15
udyn(" 3"): int(2)
ures("x"): TypeError: second(): Argument #1 ($a) must be of type int, string given, called in %s on line 16
ures("5"): int(2)
ures("5.5"): 
Deprecated: Implicit conversion from float-string "5.5" to int loses precision in %s on line 2
int(2)
ures(5): int(2)
ures(5.5): 
Deprecated: Implicit conversion from float-string "5.5" to int loses precision in %s on line 2
int(2)
ures(true): int(2)
ures(false): TypeError: second(): Argument #1 ($a) must be of type int, string given, called in %s on line 16
ures(null): 
Deprecated: strtolower(): Passing null to parameter #1 ($string) of type string is deprecated in %s on line 16
TypeError: second(): Argument #1 ($a) must be of type int, string given, called in %s on line 16
ures(7): int(2)
ures([1]): TypeError: strtolower(): Argument #1 ($string) must be of type string, array given
ures("12abc"): TypeError: second(): Argument #1 ($a) must be of type int, string given, called in %s on line 16
ures(" 3"): int(2)
ucat("x"): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 17
ucat("5"): int(7)
ucat("5.5"): 
Deprecated: Implicit conversion from float-string "5.5" to int loses precision in %s on line 3
int(7)
ucat(5): int(7)
ucat(5.5): 
Deprecated: Implicit conversion from float-string "5.5" to int loses precision in %s on line 3
int(7)
ucat(true): int(3)
ucat(false): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 17
ucat(null): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 17
ucat(7): int(9)
ucat([1]): 
Warning: Array to string conversion in %s on line 17
TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 17
ucat("12abc"): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 17
ucat(" 3"): int(5)
umeth("x"): TypeError: K::m(): Argument #1 ($a) must be of type int, string given, called in %s on line 18
umeth("5"): int(6)
umeth("5.5"): 
Deprecated: Implicit conversion from float-string "5.5" to int loses precision in %s on line 8
int(6)
umeth(5): int(6)
umeth(5.5): 
Deprecated: Implicit conversion from float 5.5 to int loses precision in %s on line 8
int(6)
umeth(true): int(2)
umeth(false): int(1)
umeth(null): TypeError: K::m(): Argument #1 ($a) must be of type int, null given, called in %s on line 18
umeth(7): int(8)
umeth([1]): TypeError: K::m(): Argument #1 ($a) must be of type int, array given, called in %s on line 18
umeth("12abc"): TypeError: K::m(): Argument #1 ($a) must be of type int, string given, called in %s on line 18
umeth(" 3"): int(4)
ustat("x"): TypeError: K::s(): Argument #1 ($a) must be of type int, string given, called in %s on line 19
ustat("5"): int(3)
ustat("5.5"): 
Deprecated: Implicit conversion from float-string "5.5" to int loses precision in %s on line 8
int(3)
ustat(5): int(3)
ustat(5.5): 
Deprecated: Implicit conversion from float 5.5 to int loses precision in %s on line 8
int(3)
ustat(true): int(3)
ustat(false): int(3)
ustat(null): TypeError: K::s(): Argument #1 ($a) must be of type int, null given, called in %s on line 19
ustat(7): int(3)
ustat([1]): TypeError: K::s(): Argument #1 ($a) must be of type int, array given, called in %s on line 19
ustat("12abc"): TypeError: K::s(): Argument #1 ($a) must be of type int, string given, called in %s on line 19
ustat(" 3"): int(3)
uloop("x"): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 20
uloop("5"): int(18)
uloop("5.5"): 
Deprecated: Implicit conversion from float-string "5.5" to int loses precision in %s on line 3

Deprecated: Implicit conversion from float-string "5.5" to int loses precision in %s on line 3

Deprecated: Implicit conversion from float-string "5.5" to int loses precision in %s on line 3
int(18)
uloop(5): int(18)
uloop(5.5): 
Deprecated: Implicit conversion from float 5.5 to int loses precision in %s on line 3

Deprecated: Implicit conversion from float 5.5 to int loses precision in %s on line 3

Deprecated: Implicit conversion from float 5.5 to int loses precision in %s on line 3
int(18)
uloop(true): int(6)
uloop(false): int(3)
uloop(null): TypeError: both(): Argument #1 ($a) must be of type int, null given, called in %s on line 20
uloop(7): int(24)
uloop([1]): TypeError: both(): Argument #1 ($a) must be of type int, array given, called in %s on line 20
uloop("12abc"): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 20
uloop(" 3"): int(12)
