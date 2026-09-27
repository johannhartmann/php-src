--TEST--
Native x64 increment and decrement through references
--DESCRIPTION--
An integer behind an untyped reference, such as an int &$value parameter, a
global or a static, is incremented in place. Overflow, non-integer values and
typed property references keep the helper's semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
function inc(int &$v): void { $v++; }
function dec_post(&$v) { return $v--; }
function pre(&$v) { return ++$v; }
class T { public int $p = 1; public ?int $q = null; }
function loop_ref(int $n) { $x = 0; $r = &$x; for ($i = 0; $i < $n; $i++) { $r++; } return $x; }
function global_inc() { global $g; $g++; return $g; }
function static_inc() { static $s = 0; $s++; return $s; }
for ($round = 0; $round < 3; $round++) {
    $a = 5; inc($a); var_dump($a);
    $m = PHP_INT_MAX; inc($m); var_dump($m);
    $s = "5"; var_dump(pre($s), $s);
    $n = null; var_dump(pre($n), $n);
    $z = "abc"; var_dump(dec_post($z), $z);
    $o = new T; $rp = &$o->p; var_dump(pre($rp), $o->p);
    $o->p = PHP_INT_MAX; try { pre($rp); } catch (Error $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
    var_dump($o->p);
    $rq = &$o->q; var_dump(pre($rq), $o->q);
    $f = 1.5; var_dump(pre($f));
    var_dump(loop_ref(1000), global_inc(), global_inc(), static_inc(), static_inc());
    $mn = PHP_INT_MIN; var_dump(dec_post($mn), $mn);
}
?>
--EXPECTF--
int(6)
float(9.223372036854776E+18)
int(6)
int(6)
int(1)
int(1)

Deprecated: Decrement on non-numeric string has no effect and is deprecated in %s on line 3
string(3) "abc"
string(3) "abc"
int(2)
int(2)
TypeError: Cannot increment a reference held by property T::$p of type int past its maximal value
int(9223372036854775807)
int(1)
int(1)
float(2.5)
int(1000)
int(1)
int(2)
int(1)
int(2)
int(-9223372036854775808)
float(-9.223372036854776E+18)
int(6)
float(9.223372036854776E+18)
int(6)
int(6)
int(1)
int(1)

Deprecated: Decrement on non-numeric string has no effect and is deprecated in %s on line 3
string(3) "abc"
string(3) "abc"
int(2)
int(2)
TypeError: Cannot increment a reference held by property T::$p of type int past its maximal value
int(9223372036854775807)
int(1)
int(1)
float(2.5)
int(1000)
int(3)
int(4)
int(3)
int(4)
int(-9223372036854775808)
float(-9.223372036854776E+18)
int(6)
float(9.223372036854776E+18)
int(6)
int(6)
int(1)
int(1)

Deprecated: Decrement on non-numeric string has no effect and is deprecated in %s on line 3
string(3) "abc"
string(3) "abc"
int(2)
int(2)
TypeError: Cannot increment a reference held by property T::$p of type int past its maximal value
int(9223372036854775807)
int(1)
int(1)
float(2.5)
int(1000)
int(5)
int(6)
int(5)
int(6)
int(-9223372036854775808)
float(-9.223372036854776E+18)
