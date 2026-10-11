--TEST--
Native direct calls do not trust the declared type of a reassigned parameter
--DESCRIPTION--
A parameter CV holds its declared type only while it holds the value its RECV received; after an assignment, a reference write, unset or a loop-carried store, an argument sending it to a typed parameter is checked like the VM's RECV.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function g2(int $a): int { return $a + 1; }
function gf(float $a): float { return $a * 2; }
function gb(bool $a): string { return $a ? "t" : "f"; }
function f_str(int $x) { $x = "abc"; return g2($x); }
function f_num(int $x) { $x = "41"; return g2($x); }
function f_flt(int $x) { $x = 2.5; return g2($x); }
function f_branch(int $x, $c) { if ($c) { $x = "zz"; } return g2($x); }
function f_ref(int $x) { $r = &$x; $r = "ref"; return g2($x); }
function f_arr(int $x) { $x = [$x]; return g2($x); }
function f_null(int $x) { $x = null; return g2($x); }
function f_unset(int $x) { unset($x); return g2($x); }
function f_loop(int $x) { $s = 0; for ($i = 0; $i < 3; $i++) { $s += g2($x); $x = $i ? "q" : 5; } return $s; }
function f_float(float $x) { $x = "1e3"; return gf($x); }
function f_bool(bool $x) { $x = []; return gb($x); }
function f_keep(int $x) { $y = $x; return g2($x) + g2($y); }
function f_after(int $x) { $r = g2($x); $x = "late"; return $r; }
$cases = [
    ['f_str', [1]], ['f_num', [1]], ['f_flt', [1]], ['f_branch', [1, true]], ['f_branch', [1, false]],
    ['f_ref', [1]], ['f_arr', [1]], ['f_null', [1]], ['f_unset', [1]], ['f_loop', [1]],
    ['f_float', [1.5]], ['f_bool', [true]], ['f_keep', [4]], ['f_after', [6]],
];
for ($round = 0; $round < 2; $round++) {
    foreach ($cases as [$f, $args]) {
        echo $f, ": ";
        try { var_dump($f(...$args)); } catch (TypeError $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
    }
}
?>
--EXPECTF--
f_str: TypeError: g2(): Argument #1 ($a) must be of type int, string given, called in %s on line 5
f_num: int(42)
f_flt: 
Deprecated: Implicit conversion from float 2.5 to int loses precision in %s on line 2
int(3)
f_branch: TypeError: g2(): Argument #1 ($a) must be of type int, string given, called in %s on line 8
f_branch: int(2)
f_ref: TypeError: g2(): Argument #1 ($a) must be of type int, string given, called in %s on line 9
f_arr: TypeError: g2(): Argument #1 ($a) must be of type int, array given, called in %s on line 10
f_null: TypeError: g2(): Argument #1 ($a) must be of type int, null given, called in %s on line 11
f_unset: 
Warning: Undefined variable $x in %s on line 12
TypeError: g2(): Argument #1 ($a) must be of type int, null given, called in %s on line 12
f_loop: TypeError: g2(): Argument #1 ($a) must be of type int, string given, called in %s on line 13
f_float: float(2000)
f_bool: TypeError: gb(): Argument #1 ($a) must be of type bool, array given, called in %s on line 15
f_keep: int(10)
f_after: int(7)
f_str: TypeError: g2(): Argument #1 ($a) must be of type int, string given, called in %s on line 5
f_num: int(42)
f_flt: 
Deprecated: Implicit conversion from float 2.5 to int loses precision in %s on line 2
int(3)
f_branch: TypeError: g2(): Argument #1 ($a) must be of type int, string given, called in %s on line 8
f_branch: int(2)
f_ref: TypeError: g2(): Argument #1 ($a) must be of type int, string given, called in %s on line 9
f_arr: TypeError: g2(): Argument #1 ($a) must be of type int, array given, called in %s on line 10
f_null: TypeError: g2(): Argument #1 ($a) must be of type int, null given, called in %s on line 11
f_unset: 
Warning: Undefined variable $x in %s on line 12
TypeError: g2(): Argument #1 ($a) must be of type int, null given, called in %s on line 12
f_loop: TypeError: g2(): Argument #1 ($a) must be of type int, string given, called in %s on line 13
f_float: float(2000)
f_bool: TypeError: gb(): Argument #1 ($a) must be of type bool, array given, called in %s on line 15
f_keep: int(10)
f_after: int(7)
