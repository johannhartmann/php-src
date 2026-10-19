--TEST--
Native compound assignments add and subtract doubles inline
--DESCRIPTION--
$x += $y and $x -= $y on a double, or a long and a double, compute the
double inline, also on a boxed register value and a method result. Long
overflow, strings, null, booleans, references, a used result, arrays,
objects and undefined operands keep the long path or the helper.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class S { function __construct(private float $v) {} function v(): float { return $this->v; } }
function acc(array $items, int $n) {
    $t = 0.0; $m = 0; $d = 10; $u = 1.5;
    for ($i = 0; $i < $n; $i++) {
        $t += $items[$i % 4]->v();
        $m += $i;
        $m -= 0.25;
        $d -= $items[$i % 4]->v();
        $u += 2;
    }
    return [$t, $m, $d, $u];
}
$items = [new S(1.0), new S(2.5), new S(-0.5), new S(4.0)];
var_dump(acc($items, 9));

function mixes() {
    $a = 1; $a += 2.5; var_dump($a);
    $b = 2.5; $b += 1; var_dump($b);
    $c = 2.5; $c -= 0.5; var_dump($c);
    $d = PHP_INT_MAX; $d += 1; var_dump($d);
    $e = PHP_INT_MIN; $e -= 1; var_dump($e);
    $f = 1.0; $f += "2.5"; var_dump($f);
    $g = "3"; $g += 1.5; var_dump($g);
    $h = null; $h += 1.5; var_dump($h);
    $k = true; $k -= 0.5; var_dump($k);
    $l = 1.5; $r = &$l; $l += 1.0; var_dump($r);
    $m = 1.5; $rr = 2.0; $q = &$rr; $m += $rr; var_dump($m);
    $n = 1.5; $o = ($n += 2.0); var_dump($n, $o);
    $p = NAN; $p += 1.0; var_dump($p);
    $s = INF; $s -= INF; var_dump($s);
    $w = -0.0; $w -= 0.0; var_dump($w);
    $x = 1.5; try { $x += []; } catch (TypeError $t) { echo $t->getMessage(), "\n"; } var_dump($x);
    $y = 7; $y |= 2; $y += 0.5; $y -= 0.5; var_dump($y);
    $z = 2.5; try { $z += new stdClass; } catch (TypeError $t) { echo $t->getMessage(), "\n"; }
    $z2 = 2.5; $z2 += $undefined; var_dump($z2);
}
mixes();

$gt = 0.0;
for ($i = 0; $i < 5; $i++) { $gt += $items[$i % 4]->v(); $gt -= 0.125; }
var_dump($gt);
function byref(&$v) { for ($i = 0; $i < 3; $i++) { $v += 0.5; } }
$br = 1; byref($br); var_dump($br);
?>
--EXPECTF--
array(4) {
  [0]=>
  float(15)
  [1]=>
  float(33.75)
  [2]=>
  float(-5)
  [3]=>
  float(19.5)
}
float(3.5)
float(3.5)
float(2)
float(9.223372036854776E+18)
float(-9.223372036854776E+18)
float(3.5)
float(4.5)
float(1.5)
float(0.5)
float(2.5)
float(3.5)
float(3.5)
float(3.5)
float(NAN)
float(NAN)
float(-0)
Unsupported operand types: float + array
float(1.5)
float(7)
Unsupported operand types: float + stdClass

Warning: Undefined variable $undefined in %s on line %d
float(2.5)
float(7.375)
float(2.5)
