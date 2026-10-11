--TEST--
Native RETURN_BY_REF of a call result reads the published result
--DESCRIPTION--
A function returning by reference that returns a direct call result notices like the VM and returns the value: the RETURN_BY_REF helper reads the result's slot, so a typed or inlined call result is published there first.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function g($x) { return $x; }
function gi(int $x): int { return $x; }
function gs($x) { return "s$x"; }
function mk(int $n): array { return [$n, $n + 1]; }
function &f0() { return g(5); }
function &f1($v) { return g($v); }
function &f2() { return gi(5); }
function &f3($v) { return gs($v); }
function &f4($v) { return strtolower($v); }
function &f5($v) { $t = g($v); return $t; }
function &f6($v) { return $v . "x"; }
function &f7($n) { return mk($n); }
function &f8($v) { return g($v) + 1; }
function &f9() { static $s = 3; return $s; }
class C {
    public static function &m($v) { return g($v); }
    public function &n($v) { return gi($v); }
}
for ($round = 0; $round < 2; $round++) {
    $a = f0(); var_dump($a);
    $b = &f0(); var_dump($b);
    var_dump(f1(2), f1("q"), f1([1]));
    $c = &f1(1.5); var_dump($c);
    $c = 9; var_dump(f1(3));
    var_dump(f2(), f3(4), f4("AB"), f5("t"), f6("u"), f7(7), f8(8));
    $d = &f9(); $d++; var_dump(f9());
    var_dump(C::m(10), (new C)->n(11));
    $e = &C::m(12); var_dump($e);
    $n = 'f1'; var_dump($n(13));
}
?>
--EXPECTF--

Notice: Only variable references should be returned by reference in %s on line 6
int(5)

Notice: Only variable references should be returned by reference in %s on line 6
int(5)

Notice: Only variable references should be returned by reference in %s on line 7

Notice: Only variable references should be returned by reference in %s on line 7

Notice: Only variable references should be returned by reference in %s on line 7
int(2)
string(1) "q"
array(1) {
  [0]=>
  int(1)
}

Notice: Only variable references should be returned by reference in %s on line 7
float(1.5)

Notice: Only variable references should be returned by reference in %s on line 7
int(3)

Notice: Only variable references should be returned by reference in %s on line 8

Notice: Only variable references should be returned by reference in %s on line 9

Notice: Only variable references should be returned by reference in %s on line 10

Notice: Only variable references should be returned by reference in %s on line 12

Notice: Only variable references should be returned by reference in %s on line 13

Notice: Only variable references should be returned by reference in %s on line 14
int(5)
string(2) "s4"
string(2) "ab"
string(1) "t"
string(2) "ux"
array(2) {
  [0]=>
  int(7)
  [1]=>
  int(8)
}
int(9)
int(4)

Notice: Only variable references should be returned by reference in %s on line 17

Notice: Only variable references should be returned by reference in %s on line 18
int(10)
int(11)

Notice: Only variable references should be returned by reference in %s on line 17
int(12)

Notice: Only variable references should be returned by reference in %s on line 7
int(13)

Notice: Only variable references should be returned by reference in %s on line 6
int(5)

Notice: Only variable references should be returned by reference in %s on line 6
int(5)

Notice: Only variable references should be returned by reference in %s on line 7

Notice: Only variable references should be returned by reference in %s on line 7

Notice: Only variable references should be returned by reference in %s on line 7
int(2)
string(1) "q"
array(1) {
  [0]=>
  int(1)
}

Notice: Only variable references should be returned by reference in %s on line 7
float(1.5)

Notice: Only variable references should be returned by reference in %s on line 7
int(3)

Notice: Only variable references should be returned by reference in %s on line 8

Notice: Only variable references should be returned by reference in %s on line 9

Notice: Only variable references should be returned by reference in %s on line 10

Notice: Only variable references should be returned by reference in %s on line 12

Notice: Only variable references should be returned by reference in %s on line 13

Notice: Only variable references should be returned by reference in %s on line 14
int(5)
string(2) "s4"
string(2) "ab"
string(1) "t"
string(2) "ux"
array(2) {
  [0]=>
  int(7)
  [1]=>
  int(8)
}
int(9)
int(5)

Notice: Only variable references should be returned by reference in %s on line 17

Notice: Only variable references should be returned by reference in %s on line 18
int(10)
int(11)

Notice: Only variable references should be returned by reference in %s on line 17
int(12)

Notice: Only variable references should be returned by reference in %s on line 7
int(13)
