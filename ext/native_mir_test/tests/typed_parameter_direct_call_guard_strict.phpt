--TEST--
Native direct calls check typed parameters of untyped arguments in strict mode
--DESCRIPTION--
The guarded inline frame of a direct user call sends a mismatching argument to the canonical call, which throws under strict_types=1 like the VM.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php declare(strict_types=1);
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
uc("5"): TypeError: second(): Argument #1 ($a) must be of type int, string given, called in %s on line 9
uc("5.5"): TypeError: second(): Argument #1 ($a) must be of type int, string given, called in %s on line 9
uc(5): TypeError: second(): Argument #1 ($a) must be of type int, float given, called in %s on line 9
uc(5.5): TypeError: second(): Argument #1 ($a) must be of type int, float given, called in %s on line 9
uc(true): TypeError: second(): Argument #1 ($a) must be of type int, true given, called in %s on line 9
uc(false): TypeError: second(): Argument #1 ($a) must be of type int, false given, called in %s on line 9
uc(null): TypeError: second(): Argument #1 ($a) must be of type int, null given, called in %s on line 9
uc(7): int(2)
uc([1]): TypeError: second(): Argument #1 ($a) must be of type int, array given, called in %s on line 9
uc("12abc"): TypeError: second(): Argument #1 ($a) must be of type int, string given, called in %s on line 9
uc(" 3"): TypeError: second(): Argument #1 ($a) must be of type int, string given, called in %s on line 9
ub("x"): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 10
ub("5"): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 10
ub("5.5"): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 10
ub(5): TypeError: both(): Argument #1 ($a) must be of type int, float given, called in %s on line 10
ub(5.5): TypeError: both(): Argument #1 ($a) must be of type int, float given, called in %s on line 10
ub(true): TypeError: both(): Argument #1 ($a) must be of type int, true given, called in %s on line 10
ub(false): TypeError: both(): Argument #1 ($a) must be of type int, false given, called in %s on line 10
ub(null): TypeError: both(): Argument #1 ($a) must be of type int, null given, called in %s on line 10
ub(7): int(8)
ub([1]): TypeError: both(): Argument #1 ($a) must be of type int, array given, called in %s on line 10
ub("12abc"): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 10
ub(" 3"): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 10
ufl("x"): TypeError: fl(): Argument #1 ($a) must be of type float, string given, called in %s on line 11
ufl("5"): TypeError: fl(): Argument #1 ($a) must be of type float, string given, called in %s on line 11
ufl("5.5"): TypeError: fl(): Argument #1 ($a) must be of type float, string given, called in %s on line 11
ufl(5): float(10)
ufl(5.5): float(11)
ufl(true): TypeError: fl(): Argument #1 ($a) must be of type float, true given, called in %s on line 11
ufl(false): TypeError: fl(): Argument #1 ($a) must be of type float, false given, called in %s on line 11
ufl(null): TypeError: fl(): Argument #1 ($a) must be of type float, null given, called in %s on line 11
ufl(7): float(14)
ufl([1]): TypeError: fl(): Argument #1 ($a) must be of type float, array given, called in %s on line 11
ufl("12abc"): TypeError: fl(): Argument #1 ($a) must be of type float, string given, called in %s on line 11
ufl(" 3"): TypeError: fl(): Argument #1 ($a) must be of type float, string given, called in %s on line 11
ubo("x"): TypeError: bo(): Argument #1 ($a) must be of type bool, string given, called in %s on line 12
ubo("5"): TypeError: bo(): Argument #1 ($a) must be of type bool, string given, called in %s on line 12
ubo("5.5"): TypeError: bo(): Argument #1 ($a) must be of type bool, string given, called in %s on line 12
ubo(5): TypeError: bo(): Argument #1 ($a) must be of type bool, float given, called in %s on line 12
ubo(5.5): TypeError: bo(): Argument #1 ($a) must be of type bool, float given, called in %s on line 12
ubo(true): int(1)
ubo(false): int(0)
ubo(null): TypeError: bo(): Argument #1 ($a) must be of type bool, null given, called in %s on line 12
ubo(7): TypeError: bo(): Argument #1 ($a) must be of type bool, int given, called in %s on line 12
ubo([1]): TypeError: bo(): Argument #1 ($a) must be of type bool, array given, called in %s on line 12
ubo("12abc"): TypeError: bo(): Argument #1 ($a) must be of type bool, string given, called in %s on line 12
ubo(" 3"): TypeError: bo(): Argument #1 ($a) must be of type bool, string given, called in %s on line 12
ust("x"): int(1)
ust("5"): int(1)
ust("5.5"): int(3)
ust(5): TypeError: st(): Argument #1 ($a) must be of type string, float given, called in %s on line 13
ust(5.5): TypeError: st(): Argument #1 ($a) must be of type string, float given, called in %s on line 13
ust(true): TypeError: st(): Argument #1 ($a) must be of type string, true given, called in %s on line 13
ust(false): TypeError: st(): Argument #1 ($a) must be of type string, false given, called in %s on line 13
ust(null): TypeError: st(): Argument #1 ($a) must be of type string, null given, called in %s on line 13
ust(7): TypeError: st(): Argument #1 ($a) must be of type string, int given, called in %s on line 13
ust([1]): TypeError: st(): Argument #1 ($a) must be of type string, array given, called in %s on line 13
ust("12abc"): int(5)
ust(" 3"): int(2)
uun("x"): TypeError: un(): Argument #1 ($a) must be of type int, string given, called in %s on line 14
uun("5"): TypeError: un(): Argument #1 ($a) must be of type int, string given, called in %s on line 14
uun("5.5"): TypeError: un(): Argument #1 ($a) must be of type int, string given, called in %s on line 14
uun(5): TypeError: un(): Argument #1 ($a) must be of type int, float given, called in %s on line 14
uun(5.5): TypeError: un(): Argument #1 ($a) must be of type int, float given, called in %s on line 14
uun(true): TypeError: un(): Argument #1 ($a) must be of type int, true given, called in %s on line 14
uun(false): TypeError: un(): Argument #1 ($a) must be of type int, false given, called in %s on line 14
uun(null): TypeError: un(): Argument #1 ($a) must be of type int, null given, called in %s on line 14
uun(7): int(7)
uun([1]): TypeError: un(): Argument #1 ($a) must be of type int, array given, called in %s on line 14
uun("12abc"): TypeError: un(): Argument #1 ($a) must be of type int, string given, called in %s on line 14
uun(" 3"): TypeError: un(): Argument #1 ($a) must be of type int, string given, called in %s on line 14
udyn("x"): TypeError: second(): Argument #1 ($a) must be of type int, string given, called in %s on line 15
udyn("5"): TypeError: second(): Argument #1 ($a) must be of type int, string given, called in %s on line 15
udyn("5.5"): TypeError: second(): Argument #1 ($a) must be of type int, string given, called in %s on line 15
udyn(5): TypeError: second(): Argument #1 ($a) must be of type int, float given, called in %s on line 15
udyn(5.5): TypeError: second(): Argument #1 ($a) must be of type int, float given, called in %s on line 15
udyn(true): TypeError: second(): Argument #1 ($a) must be of type int, true given, called in %s on line 15
udyn(false): TypeError: second(): Argument #1 ($a) must be of type int, false given, called in %s on line 15
udyn(null): TypeError: second(): Argument #1 ($a) must be of type int, null given, called in %s on line 15
udyn(7): int(2)
udyn([1]): TypeError: second(): Argument #1 ($a) must be of type int, array given, called in %s on line 15
udyn("12abc"): TypeError: second(): Argument #1 ($a) must be of type int, string given, called in %s on line 15
udyn(" 3"): TypeError: second(): Argument #1 ($a) must be of type int, string given, called in %s on line 15
ures("x"): TypeError: second(): Argument #1 ($a) must be of type int, string given, called in %s on line 16
ures("5"): TypeError: second(): Argument #1 ($a) must be of type int, string given, called in %s on line 16
ures("5.5"): TypeError: second(): Argument #1 ($a) must be of type int, string given, called in %s on line 16
ures(5): TypeError: strtolower(): Argument #1 ($string) must be of type string, float given
ures(5.5): TypeError: strtolower(): Argument #1 ($string) must be of type string, float given
ures(true): TypeError: strtolower(): Argument #1 ($string) must be of type string, true given
ures(false): TypeError: strtolower(): Argument #1 ($string) must be of type string, false given
ures(null): TypeError: strtolower(): Argument #1 ($string) must be of type string, null given
ures(7): TypeError: strtolower(): Argument #1 ($string) must be of type string, int given
ures([1]): TypeError: strtolower(): Argument #1 ($string) must be of type string, array given
ures("12abc"): TypeError: second(): Argument #1 ($a) must be of type int, string given, called in %s on line 16
ures(" 3"): TypeError: second(): Argument #1 ($a) must be of type int, string given, called in %s on line 16
ucat("x"): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 17
ucat("5"): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 17
ucat("5.5"): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 17
ucat(5): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 17
ucat(5.5): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 17
ucat(true): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 17
ucat(false): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 17
ucat(null): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 17
ucat(7): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 17
ucat([1]): 
Warning: Array to string conversion in %s on line 17
TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 17
ucat("12abc"): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 17
ucat(" 3"): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 17
umeth("x"): TypeError: K::m(): Argument #1 ($a) must be of type int, string given, called in %s on line 18
umeth("5"): TypeError: K::m(): Argument #1 ($a) must be of type int, string given, called in %s on line 18
umeth("5.5"): TypeError: K::m(): Argument #1 ($a) must be of type int, string given, called in %s on line 18
umeth(5): TypeError: K::m(): Argument #1 ($a) must be of type int, float given, called in %s on line 18
umeth(5.5): TypeError: K::m(): Argument #1 ($a) must be of type int, float given, called in %s on line 18
umeth(true): TypeError: K::m(): Argument #1 ($a) must be of type int, true given, called in %s on line 18
umeth(false): TypeError: K::m(): Argument #1 ($a) must be of type int, false given, called in %s on line 18
umeth(null): TypeError: K::m(): Argument #1 ($a) must be of type int, null given, called in %s on line 18
umeth(7): int(8)
umeth([1]): TypeError: K::m(): Argument #1 ($a) must be of type int, array given, called in %s on line 18
umeth("12abc"): TypeError: K::m(): Argument #1 ($a) must be of type int, string given, called in %s on line 18
umeth(" 3"): TypeError: K::m(): Argument #1 ($a) must be of type int, string given, called in %s on line 18
ustat("x"): TypeError: K::s(): Argument #1 ($a) must be of type int, string given, called in %s on line 19
ustat("5"): TypeError: K::s(): Argument #1 ($a) must be of type int, string given, called in %s on line 19
ustat("5.5"): TypeError: K::s(): Argument #1 ($a) must be of type int, string given, called in %s on line 19
ustat(5): TypeError: K::s(): Argument #1 ($a) must be of type int, float given, called in %s on line 19
ustat(5.5): TypeError: K::s(): Argument #1 ($a) must be of type int, float given, called in %s on line 19
ustat(true): TypeError: K::s(): Argument #1 ($a) must be of type int, true given, called in %s on line 19
ustat(false): TypeError: K::s(): Argument #1 ($a) must be of type int, false given, called in %s on line 19
ustat(null): TypeError: K::s(): Argument #1 ($a) must be of type int, null given, called in %s on line 19
ustat(7): int(3)
ustat([1]): TypeError: K::s(): Argument #1 ($a) must be of type int, array given, called in %s on line 19
ustat("12abc"): TypeError: K::s(): Argument #1 ($a) must be of type int, string given, called in %s on line 19
ustat(" 3"): TypeError: K::s(): Argument #1 ($a) must be of type int, string given, called in %s on line 19
uloop("x"): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 20
uloop("5"): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 20
uloop("5.5"): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 20
uloop(5): TypeError: both(): Argument #1 ($a) must be of type int, float given, called in %s on line 20
uloop(5.5): TypeError: both(): Argument #1 ($a) must be of type int, float given, called in %s on line 20
uloop(true): TypeError: both(): Argument #1 ($a) must be of type int, true given, called in %s on line 20
uloop(false): TypeError: both(): Argument #1 ($a) must be of type int, false given, called in %s on line 20
uloop(null): TypeError: both(): Argument #1 ($a) must be of type int, null given, called in %s on line 20
uloop(7): int(24)
uloop([1]): TypeError: both(): Argument #1 ($a) must be of type int, array given, called in %s on line 20
uloop("12abc"): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 20
uloop(" 3"): TypeError: both(): Argument #1 ($a) must be of type int, string given, called in %s on line 20
