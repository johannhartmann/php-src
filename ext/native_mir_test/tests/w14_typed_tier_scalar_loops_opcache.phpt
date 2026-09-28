--TEST--
Native x64 typed tier: scalar loops, arithmetic and increments with OPcache
--DESCRIPTION--
The typed lowering tier (ADR 0024) lowers long and double arithmetic,
increments and decrements to machine values. Mixed long/double operands,
a value used as both operands, post-decrements read by conditional jumps,
nested loops with dead outer PHIs, values stored for boxed consumers and
direct calls, deep recursion across VM stack pages and generators keep
VM results.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
memory_limit=-1
--FILE--
<?php
function mandel_row($y) {
    $out = '';
    $C = $y * 0.1 - 1.5;
    for ($x = 0; $c = $x * 0.04 - 2, $z = 0, $Z = 0, $x++ < 40;) {
        for ($r = $c, $i = $C, $k = 0; $t = $z * $z - $Z * $Z + $r, $Z = 2 * $z * $Z + $i, $z = $t, $k < 500; $k++) {
            if ($z * $z + $Z * $Z > 500000) break;
        }
        $out .= $k % 10;
    }
    return $out;
}
function countdown($n) { $s = ''; for ($y = $n; $y--;) { $s .= $y; } return $s; }
function outer_inner($n) { $out = ''; while ($n-- > 0) { for ($i = 2; $i < 6; $i++) { $out .= $i . ($i + $i); } } return $out; }
function sieve_count($n) { $count = 0; while ($n-- > 0) { $count = 0; $flags = range(0, 60); for ($i = 2; $i < 61; $i++) { if ($flags[$i] > 0) { for ($k = $i + $i; $k <= 60; $k += $i) { $flags[$k] = 0; } $count++; } } } return $count; }
function squares(float $x) { $y = $x * $x; return $y + $x - 0.5; }
function mixed_ops($n) { $t = 0.0; $x = 0.25; for ($i = 1; $i <= $n; $i++) { $t = $t + $i * $x - $i; $x = $x * 1.5; } return $t; }
function decrement($n) { $out = []; for ($i = $n; $i > 0; --$i) { $out[] = $i - 1; } return $out; }
function wide(int $depth): int {
    $v0 = $depth * 3 + 0;
    $inner = $depth > 0 ? wide($depth - 1) : 0;
    return $inner + $v0 + strlen((string) $depth);
}
function gen() { $offset = 0; yield true; for ($i = 0; $i < 100; $i++) { $offset++; if ($offset === 42) { break; } yield true; } return $offset; }
for ($round = 0; $round < 2; $round++) {
    echo mandel_row(12), "\n", mandel_row(17), "\n";
    echo countdown(5), " ", outer_inner(2), " ", sieve_count(1), "\n";
    var_dump(squares(3.0), squares(-1.5), mixed_ops(10), decrement(4));
    echo wide(1500), "\n";
    $g = gen(); foreach ($g as $v) {} echo $g->getReturn(), "\n";
    var_dump(1e308 * 10.0, (1e308 * 10.0) - (1e308 * 10.0));
}
?>
--EXPECT--
5555556666667777890382946858222350000000
5566666677777889904411000000008700000000
43210 243648510243648510 17
float(11.5)
float(0.25)
float(176.66015625)
array(4) {
  [0]=>
  int(3)
  [1]=>
  int(2)
  [2]=>
  int(1)
  [3]=>
  int(0)
}
3382144
42
float(INF)
float(NAN)
5555556666667777890382946858222350000000
5566666677777889904411000000008700000000
43210 243648510243648510 17
float(11.5)
float(0.25)
float(176.66015625)
array(4) {
  [0]=>
  int(3)
  [1]=>
  int(2)
  [2]=>
  int(1)
  [3]=>
  int(0)
}
3382144
42
float(INF)
float(NAN)
