--TEST--
Native ++ and -- of typed properties, typed references and variables
--DESCRIPTION--
Increments past the integer limits of typed properties and of references they hold raise the VM's TypeErrors; variables, undefined variables, strings, null, arrays, objects and array elements keep stock semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class T { public int $i = PHP_INT_MAX; public int $j = PHP_INT_MIN; public ?int $n = null; public int|float $f = PHP_INT_MAX; public static int $s = PHP_INT_MAX; }
function run($k) {
    $t = new T;
    try { $t->i++; } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
    try { ++$t->i; } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
    try { $t->j--; } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
    try { --$t->j; } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
    $r = &$t->i;
    try { $r++; } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
    try { $x = $r++; } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
    $q = &$t->j;
    try { $q--; } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
    try { --$q; } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
    try { T::$s++; } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
    $t->n++;
    $t->f++;
    var_dump($t->i, $t->j, $t->n, $t->f);
    $u = $undefined_inc++;
    $v = --$undefined_dec;
    $s = 'a';
    $s++;
    $z = 'Az';
    $w = ++$z;
    $nul = null;
    $nul--;
    $arr = [1];
    try { $arr++; } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
    $o = new stdClass;
    try { $o--; } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
    $big = PHP_INT_MAX;
    $big2 = $big++;
    $ref = 5;
    $rr = &$ref;
    $res = $rr++;
    $res2 = ++$rr;
    $a = ['x' => 1];
    $a['x']++;
    $a['y']++;
    var_dump($u, $v, $s, $w, $z, $nul, $big, $big2, $ref, $res, $res2, $a);
}
run(1);
run(2);
--EXPECTF--
Cannot increment property T::$i of type int past its maximal value
Cannot increment property T::$i of type int past its maximal value
Cannot decrement property T::$j of type int past its minimal value
Cannot decrement property T::$j of type int past its minimal value
Cannot increment a reference held by property T::$i of type int past its maximal value
Cannot increment a reference held by property T::$i of type int past its maximal value
Cannot decrement a reference held by property T::$j of type int past its minimal value
Cannot decrement a reference held by property T::$j of type int past its minimal value
Cannot increment property T::$s of type int past its maximal value
int(9223372036854775807)
int(-9223372036854775808)
int(1)
float(9.223372036854776E+18)

Warning: Undefined variable $undefined_inc in %s on line 19

Warning: Undefined variable $undefined_dec in %s on line 20

Warning: Decrement on type null has no effect, this will change in the next major version of PHP in %s on line 20

Deprecated: Increment on non-numeric string is deprecated, use str_increment() instead in %s on line 22

Deprecated: Increment on non-numeric string is deprecated, use str_increment() instead in %s on line 24

Warning: Decrement on type null has no effect, this will change in the next major version of PHP in %s on line 26
Cannot increment array
Cannot decrement stdClass

Warning: Undefined array key "y" in %s on line 39
NULL
NULL
string(1) "b"
string(2) "Ba"
string(2) "Ba"
NULL
float(9.223372036854776E+18)
int(9223372036854775807)
int(7)
int(5)
int(7)
array(2) {
  ["x"]=>
  int(2)
  ["y"]=>
  int(1)
}
Cannot increment property T::$i of type int past its maximal value
Cannot increment property T::$i of type int past its maximal value
Cannot decrement property T::$j of type int past its minimal value
Cannot decrement property T::$j of type int past its minimal value
Cannot increment a reference held by property T::$i of type int past its maximal value
Cannot increment a reference held by property T::$i of type int past its maximal value
Cannot decrement a reference held by property T::$j of type int past its minimal value
Cannot decrement a reference held by property T::$j of type int past its minimal value
Cannot increment property T::$s of type int past its maximal value
int(9223372036854775807)
int(-9223372036854775808)
int(1)
float(9.223372036854776E+18)

Warning: Undefined variable $undefined_inc in %s on line 19

Warning: Undefined variable $undefined_dec in %s on line 20

Warning: Decrement on type null has no effect, this will change in the next major version of PHP in %s on line 20

Deprecated: Increment on non-numeric string is deprecated, use str_increment() instead in %s on line 22

Deprecated: Increment on non-numeric string is deprecated, use str_increment() instead in %s on line 24

Warning: Decrement on type null has no effect, this will change in the next major version of PHP in %s on line 26
Cannot increment array
Cannot decrement stdClass

Warning: Undefined array key "y" in %s on line 39
NULL
NULL
string(1) "b"
string(2) "Ba"
string(2) "Ba"
NULL
float(9.223372036854776E+18)
int(9223372036854775807)
int(7)
int(5)
int(7)
array(2) {
  ["x"]=>
  int(2)
  ["y"]=>
  int(1)
}
