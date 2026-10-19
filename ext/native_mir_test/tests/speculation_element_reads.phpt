--TEST--
Native: a specialized version reads the element types its script stores
--DESCRIPTION--
With speculation on, an element read yields the type the script's own
stores guess (zend_native_element_guess()), followed through parameters
and two levels of arrays, behind an exit taken before the read: another
type, a reference, a missing key, another container or an overflow of the
assumed sum continues in the generic copy as the VM does, and a version
that keeps exiting retires to its generic copy.
--EXTENSIONS--
opcache
--ENV--
ZEND_NATIVE_SPECULATE=1
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function lsum(array $a, int $n) { $s = 0; for ($i = 0; $i < $n; $i++) { $s += $a[$i]; } return $s; }
function dsum(array $a, int $n) { $s = 0.0; for ($i = 0; $i < $n; $i++) { $s += $a[$i]; } return $s; }
function nsum(array $m) {
    $s = 0.0;
    for ($i = 0; $i < count($m); $i++) {
        for ($j = 0; $j < count($m[$i]); $j++) { $s += $m[$i][$j] * 2; }
    }
    return $s;
}
function osum($a, int $n) { $s = 0; for ($i = 0; $i < $n; $i++) { $s += $a[$i]; } return $s; }
function poke(array &$a, int $k, $v) { $a[$k] = $v; }
class Squares implements ArrayAccess {
    public function offsetExists($k): bool { return true; }
    public function offsetGet($k): mixed { echo "get $k\n"; return $k * $k; }
    public function offsetSet($k, $v): void {}
    public function offsetUnset($k): void {}
}

$l = []; for ($i = 0; $i < 8; $i++) { $l[] = $i * 3; }
$d = []; for ($i = 0; $i < 8; $i++) { $d[] = $i * 0.25; }
$m = []; for ($i = 0; $i < 3; $i++) { $r = []; for ($j = 0; $j < 3; $j++) { $r[] = $i + $j * 0.5; } $m[] = $r; }
var_dump(lsum($l, 8), dsum($d, 8), nsum($m), osum($l, 8));

echo "-- other elements\n";
$x1 = $l; poke($x1, 3, 1.5); var_dump(lsum($x1, 8));
$x2 = $l; poke($x2, 2, "7"); var_dump(lsum($x2, 8));
$x3 = $l; poke($x3, 5, null); var_dump(lsum($x3, 8));
$x4 = $l; $ref = &$x4[4]; $ref = 100; var_dump(lsum($x4, 8)); unset($ref);
$x5 = $d; poke($x5, 1, 2); var_dump(dsum($x5, 8));
$h = [7 => 7, 3 => 3, 0 => 0, 1 => 1, 2 => 2, 4 => 4, 5 => 5, 6 => 6]; var_dump(lsum($h, 8));
$y1 = $m; poke($y1[1], 2, 10); var_dump(nsum($y1));

echo "-- missing keys\n";
var_dump(lsum($l, 9));
$y2 = $m; unset($y2[1][1]); var_dump(@nsum($y2));

echo "-- overflow\n";
$big = [PHP_INT_MAX, 1, 2]; var_dump(lsum($big, 3));

echo "-- other containers\n";
var_dump(osum("12345678", 8));
var_dump(osum(new Squares, 3));

echo "-- retirement\n";
$z = $l; poke($z, 7, 0.5);
for ($k = 0; $k < 40; $k++) { $res = lsum($k % 2 ? $l : $z, 8); }
var_dump($res, lsum($l, 8), lsum($z, 8));
?>
--EXPECTF--
int(84)
float(7)
float(27)
int(84)
-- other elements
float(76.5)
int(85)
int(69)
int(172)
float(8.75)
int(28)
float(43)
-- missing keys

Warning: Undefined array key 8 in %s on line 2
int(84)
float(20)
-- overflow
float(9.223372036854776E+18)
-- other containers
int(36)
get 0
get 1
get 2
int(5)
-- retirement
int(84)
int(84)
float(63.5)
