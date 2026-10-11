--TEST--
Native direct calls with typed by-reference parameters keep their frame
--DESCRIPTION--
A typed by-reference parameter is checked against its referent only at run
time. Such a call is neither planned nor emitted as an effect-closed inline
body, which would read the referent unchecked: coercion, TypeError and writes
through the reference behave like the VM.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function rd(int &$x): int { return $x + 1; }
function rdb(bool &$x): bool { return !$x; }
function wr(int &$x): int { $x = $x + 1; return $x; }
function both(int &$x, int $y): int { return $x < $y ? 1 : 0; }
function untyped_ref(&$x) { return $x; }
function caller($seed) {
    $a = $seed; $b = true; $s = 0; $str = "7"; $f = 2.5;
    for ($i = 0; $i < 3; $i++) {
        $s += rd($a);
        $s += rdb($b) ? 10 : 20;
        $s += wr($a);
        $s += both($a, 9);
        $s += untyped_ref($a);
    }
    try { $s += rd($str); } catch (TypeError $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
    var_dump($str);
    try { $s += rd($f); } catch (TypeError $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
    var_dump($f);
    return [$s, $a];
}
for ($r = 0; $r < 3; $r++) {
    var_dump(caller(5), caller(PHP_INT_MAX - 10));
    $n = null;
    try { var_dump(rd($n)); } catch (TypeError $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
    var_dump($n);
}
?>
--EXPECTF--
int(7)

Deprecated: Implicit conversion from float 2.5 to int loses precision in %s on line 2
int(2)
int(7)

Deprecated: Implicit conversion from float 2.5 to int loses precision in %s on line 2
int(2)
array(2) {
  [0]=>
  int(137)
  [1]=>
  int(8)
}
array(2) {
  [0]=>
  float(8.301034833169298E+19)
  [1]=>
  int(9223372036854775800)
}
TypeError: rd(): Argument #1 ($x) must be of type int, null given, called in %s on line 25
NULL
int(7)

Deprecated: Implicit conversion from float 2.5 to int loses precision in %s on line 2
int(2)
int(7)

Deprecated: Implicit conversion from float 2.5 to int loses precision in %s on line 2
int(2)
array(2) {
  [0]=>
  int(137)
  [1]=>
  int(8)
}
array(2) {
  [0]=>
  float(8.301034833169298E+19)
  [1]=>
  int(9223372036854775800)
}
TypeError: rd(): Argument #1 ($x) must be of type int, null given, called in %s on line 25
NULL
int(7)

Deprecated: Implicit conversion from float 2.5 to int loses precision in %s on line 2
int(2)
int(7)

Deprecated: Implicit conversion from float 2.5 to int loses precision in %s on line 2
int(2)
array(2) {
  [0]=>
  int(137)
  [1]=>
  int(8)
}
array(2) {
  [0]=>
  float(8.301034833169298E+19)
  [1]=>
  int(9223372036854775800)
}
TypeError: rd(): Argument #1 ($x) must be of type int, null given, called in %s on line 25
NULL
