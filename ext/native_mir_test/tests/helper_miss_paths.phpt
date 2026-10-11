--TEST--
Native value helpers after a fast-path miss
--DESCRIPTION--
VERIFY_RETURN_TYPE of polymorphic, nullable, coerced and undefined returns, CONCAT of non-string operands, array writes and reads of missing keys, and compound assignments and increments of object and static properties keep stock semantics after their fast paths miss.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
interface Shape {}
class Circle implements Shape {}
class Square implements Shape {}
class Weird {}
class Str { function __toString(): string { return "S"; } }
class Bad { function __toString(): string { throw new Exception("bad"); } }
function shape(int $i): Shape { return $i % 2 ? new Circle : new Square; }
function maybe(int $i): ?Shape { return $i % 3 == 0 ? null : shape($i); }
function wrong(int $i): Shape { return $i > 1 ? new Weird : new Circle; }
function &refret(array &$a): int { return $a[0]; }
function coerce(string $s): int { return $s; }
function union(int $i): int|string { $v = $i % 2 ? "s$i" : $i; return $v; }
function undef(): ?int { return $x; }
function undef2(): int { return $y; }
function concats($i) {
    $r = [];
    $r[] = 'x' . $i;
    $r[] = $i . 'y';
    $r[] = 1.5 . 'z';
    $r[] = true . '|' . null . '|' . false;
    $r[] = 'o' . new Str;
    $a = ['k' => 'v'];
    $r[] = 'arr:' . $a['k'];
    try { $r[] = 'b' . new Bad; } catch (Exception $e) { $r[] = $e->getMessage(); }
    $ref = 'R';
    $alias = &$ref;
    $r[] = $alias . $i;
    $r[] = $undefined_cv . '!';
    return $r;
}
function dims($i) {
    $a = [];
    for ($j = 0; $j < 3; $j++) {
        $a[$j] = $j;
        $a["k$j"] = $j;
        $a["7$j"] = $j;
    }
    $a[PHP_INT_MAX] = 1;
    try { $a[] = 2; } catch (Error $e) { echo $e->getMessage(), "\n"; }
    $r = [$a];
    $r[] = $a[99] ?? 'none';
    $r[] = @$a['missing'];
    $r[] = $a['k1'];
    $r[] = $a['71'];
    $r[] = isset($a['k9']);
    $missing = $a['nokey'];
    $missing2 = $a[123];
    $b = [1, 2];
    $b[5] = 5;
    $b['5'] = 'five';
    $b[-1] = 'neg';
    $r[] = $b;
    $s = 'str';
    $r[] = $s[1];
    $r[] = $s[-1];
    $r[] = @$s[10];
    $n = null;
    $r[] = $n['x'] ?? 'null';
    $key = 'kk';
    $c = ['kk' => 1];
    $kr = &$key;
    $r[] = $c[$kr];
    $r[] = $c[$kr . 'x'] ?? 'nx';
    return $r;
}
function counters($i) {
    $o = new class { public $n = 0; public int $t = 0; public static $s = 0; public static int $ts = 5; };
    for ($j = 0; $j < 3; $j++) {
        $o->n++;
        ++$o->n;
        $o->n--;
        $o->t++;
        $o->n += 10;
        $o->n -= 1;
        $o::$s += 2;
        $o::$ts -= 1;
        $o::$s .= 'x';
    }
    $o->t = PHP_INT_MAX;
    try { $o->t++; } catch (Error $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
    $o->n = PHP_INT_MAX;
    $o->n++;
    $o->n = PHP_INT_MIN;
    $o->n -= 1;
    return [$o->n, $o->t, $o::$s, $o::$ts];
}
for ($i = 0; $i < 3; $i++) {
    echo get_class(shape($i)), ' ', json_encode(maybe($i)), "\n";
    try { echo get_class(wrong($i)), "\n"; } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
    $arr = [5];
    $x = &refret($arr);
    $x = 6;
    echo json_encode($arr), ' ', coerce("4$i"), ' ', json_encode(union($i)), "\n";
    try { echo coerce("x"), "\n"; } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
    var_dump(undef());
    try { var_dump(undef2()); } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
    echo json_encode(concats($i)), "\n";
    echo json_encode(dims($i)), "\n";
    echo json_encode(counters($i)), "\n";
}
--EXPECTF--
Square null
Circle
[6] 40 0
coerce(): Return value must be of type int, string returned

Warning: Undefined variable $x in %s on line 14
NULL

Warning: Undefined variable $y in %s on line 15
undef2(): Return value must be of type int, null returned

Warning: Undefined variable $undefined_cv in %s on line 29
["x0","0y","1.5z","1||","oS","arr:v","bad","R0","!"]
Cannot add element to the array as the next element is already occupied

Warning: Undefined array key "nokey" in %s on line 47

Warning: Undefined array key 123 in %s on line 48
[{"0":0,"k0":0,"70":0,"1":1,"k1":1,"71":1,"2":2,"k2":2,"72":2,"9223372036854775807":1},"none",null,1,1,false,{"0":1,"1":2,"5":"five","-1":"neg"},"t","r","","null",1,"nx"]

Warning: A non-numeric value encountered in %s on line 76

Warning: A non-numeric value encountered in %s on line 76
TypeError: Cannot increment property class@anonymous::$t of type int past its maximal value
[-9.223372036854776e+18,9223372036854775807,"6x",2]
Circle {}
Circle
[6] 41 "s1"
coerce(): Return value must be of type int, string returned

Warning: Undefined variable $x in %s on line 14
NULL

Warning: Undefined variable $y in %s on line 15
undef2(): Return value must be of type int, null returned

Warning: Undefined variable $undefined_cv in %s on line 29
["x1","1y","1.5z","1||","oS","arr:v","bad","R1","!"]
Cannot add element to the array as the next element is already occupied

Warning: Undefined array key "nokey" in %s on line 47

Warning: Undefined array key 123 in %s on line 48
[{"0":0,"k0":0,"70":0,"1":1,"k1":1,"71":1,"2":2,"k2":2,"72":2,"9223372036854775807":1},"none",null,1,1,false,{"0":1,"1":2,"5":"five","-1":"neg"},"t","r","","null",1,"nx"]

Warning: A non-numeric value encountered in %s on line 76

Warning: A non-numeric value encountered in %s on line 76

Warning: A non-numeric value encountered in %s on line 76
TypeError: Cannot increment property class@anonymous::$t of type int past its maximal value
[-9.223372036854776e+18,9223372036854775807,"12x",-1]
Square {}
wrong(): Return value must be of type Shape, Weird returned
[6] 42 2
coerce(): Return value must be of type int, string returned

Warning: Undefined variable $x in %s on line 14
NULL

Warning: Undefined variable $y in %s on line 15
undef2(): Return value must be of type int, null returned

Warning: Undefined variable $undefined_cv in %s on line 29
["x2","2y","1.5z","1||","oS","arr:v","bad","R2","!"]
Cannot add element to the array as the next element is already occupied

Warning: Undefined array key "nokey" in %s on line 47

Warning: Undefined array key 123 in %s on line 48
[{"0":0,"k0":0,"70":0,"1":1,"k1":1,"71":1,"2":2,"k2":2,"72":2,"9223372036854775807":1},"none",null,1,1,false,{"0":1,"1":2,"5":"five","-1":"neg"},"t","r","","null",1,"nx"]

Warning: A non-numeric value encountered in %s on line 76

Warning: A non-numeric value encountered in %s on line 76

Warning: A non-numeric value encountered in %s on line 76
TypeError: Cannot increment property class@anonymous::$t of type int past its maximal value
[-9.223372036854776e+18,9223372036854775807,"18x",-4]
