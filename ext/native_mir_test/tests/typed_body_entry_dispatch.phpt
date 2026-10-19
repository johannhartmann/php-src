--TEST--
Native: a Zend entry runs its member's typed body
--DESCRIPTION--
Calls that do not reach a typed body through a typed call (from the main
script, from code with untyped values, from internal callbacks) enter the
Zend entry. When the frame holds exactly the declared arguments with the
types the typed body takes, the entry runs the typed body; other argument
types, missing or extra arguments and a failing body run the general code.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function isum(int $n) { $s = 0; for ($i = 0; $i < $n; $i++) { $s = ($s + ($i & 255)) & 0xFFFF; } return $s; }
function fmul(float $x, int $n): float { $p = 1.0; for ($i = 0; $i < $n; $i++) { $p *= $x; } return $p; }
function positive(int $n): bool { return $n > 0; }
function same($v) { return $v; }
function num(int|float $v) { return $v + 1; }
function grow(int $n) { $a = 1; for ($i = 0; $i < $n; $i++) { $a = $a * 3 + 1; } return $a; }
function dflt(int $a, int $b = 7) { return $a * 100 + $b; }
function strict_int(int $n): int { return $n * 2; }
class K { static function twice(int $n) { return $n + $n; } function plus(int $n) { return $n + 1; } }
$f = fn(int $x): int => $x * 3;

var_dump(isum(1000), isum(0));
var_dump(fmul(1.5, 4), fmul(2.0, 10));
var_dump(positive(3), positive(-3));
$o = new stdClass;
var_dump(same(5), same("str" . mt_rand(1, 1)), same([1, 2]), same($o) === $o, same(null));
var_dump(num(1), num(1.5));
var_dump(grow(39), grow(40), grow(41));
var_dump(dflt(3), dflt(3, 4));
var_dump(strict_int("21"), strict_int(2.0));
try { strict_int("x"); } catch (TypeError $e) { echo get_class($e), "\n"; }
try { isum(); } catch (ArgumentCountError $e) { echo get_class($e), "\n"; }
var_dump(isum(10, 99));
var_dump(K::twice(21), (new K)->plus(41));
var_dump(array_map('isum', [10, 20]), array_map($f, [1, 2, 3]));
var_dump(array_map('same', ['a', 'b']));
$name = 'isum';
var_dump($name(100), call_user_func('fmul', 3.0, 2));
var_dump(fmul(2, 3));
?>
--EXPECT--
int(59180)
int(0)
float(5.0625)
float(1024)
bool(true)
bool(false)
int(5)
string(4) "str1"
array(2) {
  [0]=>
  int(1)
  [1]=>
  int(2)
}
bool(true)
NULL
int(2)
float(2.5)
int(6078832729528464400)
float(1.8236498188585394E+19)
float(5.470949456575618E+19)
int(307)
int(304)
int(42)
int(4)
TypeError
ArgumentCountError
int(45)
int(42)
int(42)
array(2) {
  [0]=>
  int(45)
  [1]=>
  int(190)
}
array(3) {
  [0]=>
  int(3)
  [1]=>
  int(6)
  [2]=>
  int(9)
}
array(2) {
  [0]=>
  string(1) "a"
  [1]=>
  string(1) "b"
}
int(4950)
float(9)
float(8)
