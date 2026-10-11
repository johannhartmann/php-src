--TEST--
Native generators keep loop values of every kind across resumes (without opcache)
--DESCRIPTION--
String, array, float and bool loop values, switch and do-while bodies, closures, integer overflow, many live accumulators and early returns survive generator suspension and resume in typed loops.
--INI--
opcache.enable=0
opcache.enable_cli=0
--FILE--
<?php
function run($name, $gen, $send = null, $throwAt = null) {
    echo "== $name\n";
    $c = 0;
    try {
        foreach ($gen as $k => $v) {
            echo $k, " => ", var_export($v, true), "\n";
            if ($send !== null && $v === $send[0]) {
                $r = $gen->send($send[1]);
                echo "send -> ", var_export($r, true), "\n";
            }
            if ($throwAt !== null && $v === $throwAt) {
                try {
                    $r = $gen->throw(new Exception("boom"));
                    echo "throw -> ", var_export($r, true), "\n";
                } catch (Exception $e) {
                    echo "escaped: ", $e->getMessage(), "\n";
                }
            }
            if (++$c > 30) { echo "runaway\n"; break; }
        }
        echo "ret: ", var_export($gen->getReturn(), true), "\n";
    } catch (Throwable $e) {
        echo "outer: ", get_class($e), " ", $e->getMessage(), "\n";
    }
}

function strs(int $n) { $s = "a"; for ($i = 0; $i < $n; $i++) { $s .= "b"; $r = yield $s; if ($r !== null) { $s = $r; yield strlen($s); } } return $s; }
function arrs(int $n) { $a = []; for ($i = 0; $i < $n; $i++) { $a[] = $i; $r = yield count($a); if ($r !== null) { $a[] = $r; yield implode(",", $a); } } return $a; }
function sw(int $n) { for ($i = 0; $i < $n; $i++) { switch ($i % 3) { case 0: $r = yield "zero$i"; break; case 1: $r = yield "one$i"; if ($r) { yield "r$r"; } break; default: yield "two$i"; } } return $i; }
function dw(int $n) { $i = 0; do { $r = yield $i; if ($r !== null) { $i += $r; yield "+$r"; } $i++; } while ($i < $n); return $i; }
function recv() { $total = 0; while (($x = yield $total) !== null) { $total += $x; } return $total; }
function ovf(int $n) { $x = PHP_INT_MAX - 2; for ($i = 0; $i < $n; $i++) { $x = $x + 1; $r = yield $x; if ($r !== null) { yield $r; } } return $x; }
function many(int $n) { $a = 1; $b = 2; $c = 3; $d = 4; $e = 5; $f = 6; $g = 7; $h = 8; for ($i = 0; $i < $n; $i++) { $a += $b; $b += $c; $c += $d; $d += $e; $e += $f; $f += $g; $g += $h; $h += $i; $r = yield $a + $b + $c + $d + $e + $f + $g + $h; if ($r !== null) { $a = $r; yield "$a $b $c $d $e $f $g $h"; } } return [$a, $b, $c, $d, $e, $f, $g, $h]; }
function ret(int $n) { for ($i = 0; $i < $n; $i++) { $r = yield $i; if ($r === "stop") { return "stopped at $i"; } } return "done"; }
function tc(int $n) { for ($i = 0; $i < $n; $i++) { try { $r = yield $i; if ($r !== null) { yield "s$r"; } } catch (Exception $e) { yield "c$i"; } } return $i; }
function fl2(float $f, int $n) { for ($i = 0; $i < $n; $i++) { $f = $f * 1.5; $r = yield $f; if ($r !== null) { $f = $r; yield $f; } } return $f; }
function bl(bool $b, int $n) { for ($i = 0; $i < $n; $i++) { $b = !$b; $r = yield $i => $b; if ($r !== null) { yield $b && $r; } } return $b; }
$cl = function (int $n) { for ($i = 0; $i < $n; $i++) { $r = yield $i * 7; if ($r !== null) { yield "cl$r"; } } return $i; };
function gfrom(int $n) { for ($k = 0; $k < $n; $k++) { $r = yield from [$k, $k + 10]; $s = yield "after$k"; if ($s !== null) { yield "s$s"; } } return $k; }
function nest3(int $n) { for ($i = 0; $i < $n; $i++) { for ($j = 0; $j < 2; $j++) { for ($k = 0; $k < 2; $k++) { if (($i + $j + $k) % 2 == 0) { $r = yield "$i$j$k"; if ($r !== null) { yield "r$r"; } } } } } return $i * 100 + $j * 10 + $k; }

run("strs", strs(3), ["abb", "zz"]);
run("arrs", arrs(3), [2, 9]);
run("sw", sw(6), ["one1", 5]);
run("dw", dw(5), [1, 2]);
$g = recv(); $g->current(); $g->send(3); $g->send(4); $g->send(5); $g->send(null); var_dump($g->getReturn());
run("ovf", ovf(4), [PHP_INT_MAX, "o"]);
run("many", many(4), [36, 1]);
run("ret", ret(5), [2, "stop"]);
run("tc", tc(4), [1, "x"], 2);
run("fl2", fl2(1.0, 3), [1.5, 10.0]);
run("bl", bl(false, 4), [true, 1]);
run("closure", $cl(3), [7, "C"]);
run("gfrom", gfrom(2), ["after0", "S"]);
run("nest3", nest3(3), ["011", "N"]);
?>
--EXPECT--
== strs
0 => 'ab'
1 => 'abb'
send -> 2
3 => 'zzb'
ret: 'zzb'
== arrs
0 => 1
1 => 2
send -> '0,1,9'
3 => 4
ret: array (
  0 => 0,
  1 => 1,
  2 => 9,
  3 => 2,
)
== sw
0 => 'zero0'
1 => 'one1'
send -> 'r5'
3 => 'two2'
4 => 'zero3'
5 => 'one4'
6 => 'two5'
ret: 6
== dw
0 => 0
1 => 1
send -> '+2'
3 => 4
ret: 5
int(12)
== ovf
0 => 9223372036854775806
1 => 9223372036854775807
send -> 'o'
3 => 9.223372036854776E+18
4 => 9.223372036854776E+18
ret: 9.223372036854776E+18
== many
0 => 71
1 => 140
2 => 274
3 => 531
ret: array (
  0 => 48,
  1 => 64,
  2 => 80,
  3 => 96,
  4 => 103,
  5 => 83,
  6 => 43,
  7 => 14,
)
== ret
0 => 0
1 => 1
2 => 2
send -> NULL
ret: 'stopped at 2'
== tc
0 => 0
1 => 1
send -> 'sx'
3 => 2
throw -> 'c2'
5 => 3
ret: 4
== fl2
0 => 1.5
send -> 10.0
2 => 15.0
3 => 22.5
ret: 22.5
== bl
0 => true
send -> true
1 => false
2 => true
send -> true
3 => false
ret: false
== closure
0 => 0
1 => 7
send -> 'clC'
3 => 14
ret: 3
== gfrom
0 => 0
1 => 10
0 => 'after0'
send -> 'sS'
0 => 1
1 => 11
2 => 'after1'
ret: 2
== nest3
0 => '000'
1 => '011'
send -> 'rN'
3 => '101'
4 => '110'
5 => '200'
6 => '211'
ret: 322
