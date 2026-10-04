--TEST--
Native x64 typed tier compares doubles with IEEE semantics without finite facts
--DESCRIPTION--
The typed tier lowers comparisons of doubles that may be NaN or infinite to
machine comparisons. An unordered operand makes <, <=, == and === false and
!= true, as in the VM. Typed bodies that branch and return a double argument
keep the result register free.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function cmp(float $a, float $b) { return [$a < $b, $a <= $b, $a == $b, $a != $b, $a > $b, $a >= $b, $a === $b]; }
function loop(array $v) { $c = 0; $n = count($v); for ($i = 0; $i < $n; $i++) { for ($j = 0; $j < $n; $j++) { $x = $v[$i] * 1.0; $y = $v[$j] * 1.0; if ($x < $y) $c += 1; if ($x <= $y) $c += 10; if ($x == $y) $c += 100; if ($x != $y) $c += 1000; } } return $c; }
function sq($x) { $s = 0.0; for ($i = 0; $i < 3; $i++) { $s = $s + $x; if ($s > 1e308) { return "big"; } } return $s < INF ? "fin" : "inf"; }
$vals = [NAN, INF, -INF, 0.0, -0.0, 1.5, -1.5];
for ($r = 0; $r < 2; $r++) {
    $out = [];
    foreach ($vals as $a) foreach ($vals as $b) $out[] = implode('', array_map('intval', cmp($a, $b)));
    echo implode(' ', $out), "\n", loop($vals), " ", sq(1e308), " ", sq(1.0), " ", sq(NAN), "\n";
}
function pick(float $a, float $b, int $k): float { if ($k > 1) { return $a; } return $b; }
function larger(float $a, float $b): float { if ($a > $b) { return $a; } return $b; }
$m = 0.0; for ($i = 0; $i < 3; $i++) { $m = pick(1.0, $m, $i); $m = larger(3.0, $m); }
var_dump($m, larger(NAN, 2.0), larger(2.0, NAN), pick(INF, -INF, 0));
?>
--EXPECT--
0001000 0001000 0001000 0001000 0001000 0001000 0001000 0001000 0110011 0001110 0001110 0001110 0001110 0001110 0001000 1101000 0110011 1101000 1101000 1101000 1101000 0001000 1101000 0001110 0110011 0110011 1101000 0001110 0001000 1101000 0001110 0110011 0110011 1101000 0001110 0001000 1101000 0001110 0001110 0001110 0110011 0001110 0001000 1101000 0001110 1101000 1101000 1101000 0110011
42034 big fin inf
0001000 0001000 0001000 0001000 0001000 0001000 0001000 0001000 0110011 0001110 0001110 0001110 0001110 0001110 0001000 1101000 0110011 1101000 1101000 1101000 1101000 0001000 1101000 0001110 0110011 0110011 1101000 0001110 0001000 1101000 0001110 0110011 0110011 1101000 0001110 0001000 1101000 0001110 0001110 0001110 0110011 0001110 0001000 1101000 0001110 1101000 1101000 1101000 0110011
42034 big fin inf
float(3)
float(2)
float(NAN)
float(-INF)
