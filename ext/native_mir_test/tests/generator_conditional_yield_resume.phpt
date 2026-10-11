--TEST--
Native generators resume conditional yields in typed loops
--DESCRIPTION--
A yield whose continuation begins a merge block publishes the values that the block's PHIs receive on its edge. Suspend liveness counts an e-SSA pi only on its own edge, and a comparison is not fused into a resume block's branch, whose terminal reloads the resumed values. A materialized resume value keeps a separate reload use.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function b(int $n) { for ($i = 0; $i < $n; $i++) { yield $i * 2; if ($i == 1) { yield "x"; } } }
function c(int $n) { for ($i = 0; $i < $n; $i++) { if ($i == 1) { yield "x"; } } yield $i; }
function d(int $n) { $i = 0; while ($i < $n) { if ($i == 1) { yield "x"; } $i++; } yield $i; }
function cond2(int $n) { for ($i = 0; $i < $n; $i++) { if ($i % 2 == 0) { yield "even$i"; } else { yield "odd$i"; } } yield $i; }
function k1(int $n) { $x = 0; for ($i = 0; $i < $n; $i++) { if ($i % 2) { $x = 1; yield "a$i"; } else { $x = 2; yield "b$i"; } yield $x + $i; } return $x; }
function k2(int $n) { for ($i = 0; $i < $n; $i++) { $x = 5; if ($i == 1) { $x = 7; yield "y"; } yield $x * $i; } }
function k3(int $n) { $f = 0.0; for ($i = 0; $i < $n; $i++) { if ($i > 0) { $f = 2.5; yield "f"; } else { $f = 1.5; } yield $f + $i; } }
function k4(int $n) { $b = false; for ($i = 0; $i < $n; $i++) { if ($i == 2) { $b = true; yield "t"; } yield $b ? "B$i" : "n$i"; } }
function k5(int $n) { $i = 0; while ($i < $n) { $j = $i * 2; if ($j > 2) { yield "big"; } $i = $j + 1; yield $i; } }
function k6(int $n) { for ($i = 0, $s = 0; $i < $n; $i++) { $s += $i; if ($s > 1) { $s = 0; yield "reset$i"; } yield $s; } }
function acc(int $n) { $sum = 0; for ($i = 1; $i <= $n; $i++) { $sum += $i; $r = yield $sum; if ($r !== null) { $sum += $r; yield $sum; } } return $sum; }

foreach (['b', 'c', 'd', 'cond2', 'k1', 'k2', 'k3', 'k4', 'k5', 'k6'] as $f) {
    $c = 0;
    echo "$f:";
    foreach ($f(5) as $k => $v) {
        echo " $k=", var_export($v, true);
        if (++$c > 30) { echo " runaway"; break; }
    }
    echo "\n";
}
$g = acc(4);
foreach ($g as $v) {
    echo $v, "\n";
    if ($v === 3) $g->send(100);
}
var_dump($g->getReturn());
?>
--EXPECT--
b: 0=0 1=2 2='x' 3=4 4=6 5=8
c: 0='x' 1=5
d: 0='x' 1=5
cond2: 0='even0' 1='odd1' 2='even2' 3='odd3' 4='even4' 5=5
k1: 0='b0' 1=2 2='a1' 3=2 4='b2' 5=4 6='a3' 7=4 8='b4' 9=6
k2: 0=0 1='y' 2=7 3=10 4=15 5=20
k3: 0=1.5 1='f' 2=3.5 3='f' 4=4.5 5='f' 6=5.5 7='f' 8=6.5
k4: 0='n0' 1='n1' 2='t' 3='B2' 4='B3' 5='B4'
k5: 0=1 1=3 2='big' 3=7
k6: 0=0 1=1 2='reset2' 3=0 4='reset3' 5=0 6='reset4' 7=0
1
3
106
110
int(110)
