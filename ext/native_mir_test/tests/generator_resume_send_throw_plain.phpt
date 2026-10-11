--TEST--
Native generators in typed loops handle send, throw and yield from (without opcache)
--DESCRIPTION--
Typed float, bool and int loops, nested loops, keys, yield from, throw() into the generator, finally blocks, foreach bodies and methods resume with their loop state intact after send() and throw().
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
    } catch (Exception $e) {
        echo "outer: ", $e->getMessage(), "\n";
    }
}

function fl(float $n) { for ($x = 0.5; $x < $n; $x += 1.0) { $r = yield $x * 2; if ($r !== null) { yield $r; } } }
function bo(bool $b, int $n) { $f = $b; for ($i = 0; $i < $n; $i++) { $f = !$f; $r = yield $f; if ($r) { yield "got"; } } return $f; }
function nested(int $n) { for ($i = 0; $i < $n; $i++) { for ($j = 0; $j < $i; $j++) { $r = yield $i * 10 + $j; if ($r !== null) { yield "$i/$j:$r"; } } } }
function keyed(int $n) { for ($i = 0; $i < $n; $i++) { $r = yield $i => $i * $i; if ($r !== null) { yield "k$i" => $r; } } }
function inner(int $n) { for ($i = 0; $i < $n; $i++) { $r = yield $i; if ($r !== null) { yield "in:$r"; } } return $n; }
function outer(int $n) { for ($k = 0; $k < 2; $k++) { $ret = yield from inner($n); yield "ret$k:$ret"; } }
function thrower(int $n) { for ($i = 0; $i < $n; $i++) { try { yield $i; } catch (Exception $e) { yield "caught$i"; } } }
function fin(int $n) { for ($i = 0; $i < $n; $i++) { try { $r = yield $i; if ($r !== null) { yield $r; } } finally { echo "finally $i\n"; } } }
function wh(int $n) { $i = 0; while (true) { if ($i >= $n) break; $r = yield $i; $i++; if ($r === null) continue; yield "w$r"; } }
function acc(int $n) { $sum = 0; for ($i = 1; $i <= $n; $i++) { $sum += $i; $r = yield $sum; if ($r !== null) { $sum += $r; yield $sum; } } return $sum; }
function fe(array $a, int $m) { foreach ($a as $k => $v) { $r = yield $k => $v * $m; if ($r !== null) { yield $r; } } }
class C { private int $base = 7; function gen(int $n) { for ($i = 0; $i < $n; $i++) { $r = yield $this->base + $i; if ($r !== null) { yield $r; } } } }
function dbl(int $n) { for ($i = 0; $i < $n; $i++) { yield $i; yield -$i; yield $i * 3; } }
function cond2(int $n) { for ($i = 0; $i < $n; $i++) { if ($i % 2 == 0) { yield "even$i"; } else { yield "odd$i"; } } yield $i; }

run("float", fl(3.0), [1.0, 2.5]);
run("float2", fl(3.0));
run("bool", bo(true, 4), [false, 1]);
$g = bo(false, 3); foreach ($g as $v) { var_dump($v); } var_dump($g->getReturn());
run("nested", nested(4), [21, "S"]);
run("keyed", keyed(4), [4, "K"]);
run("yieldfrom", outer(3), [1, "F"]);
run("throw", thrower(4), null, 1);
run("finally", fin(3), [1, "Z"]);
run("while", wh(4), [1, "Q"]);
$g = acc(4); foreach ($g as $v) { echo $v, "\n"; if ($v === 3) $g->send(100); } var_dump($g->getReturn());
run("foreach", fe([3 => 1, 5 => 2, 9 => 3], 4), [8, "E"]);
run("method", (new C)->gen(3), [8, "M"]);
run("dbl", dbl(3));
run("cond2", cond2(5));
$g = nested(3); var_dump($g->current()); var_dump($g->send("a")); var_dump($g->send(null)); var_dump($g->send("b")); var_dump($g->send(null)); var_dump($g->valid());
?>
--EXPECT--
== float
0 => 1.0
send -> 2.5
2 => 3.0
3 => 5.0
== float2
0 => 1.0
1 => 3.0
2 => 5.0
== bool
0 => false
send -> 'got'
2 => true
3 => false
send -> 'got'
5 => true
bool(true)
bool(false)
bool(true)
bool(true)
== nested
0 => 10
1 => 20
2 => 21
send -> '2/1:S'
4 => 30
5 => 31
6 => 32
== keyed
0 => 0
1 => 1
2 => 4
send -> 'K'
3 => 9
== yieldfrom
0 => 0
1 => 1
send -> 'in:F'
3 => 2
0 => 'ret0:3'
0 => 0
1 => 1
send -> 'in:F'
3 => 2
1 => 'ret1:3'
== throw
0 => 0
1 => 1
throw -> 'caught1'
3 => 2
4 => 3
== finally
0 => 0
finally 0
1 => 1
send -> 'Z'
finally 1
3 => 2
finally 2
== while
0 => 0
1 => 1
send -> 'wQ'
3 => 2
4 => 3
1
3
106
110
int(110)
== foreach
3 => 4
5 => 8
send -> 'E'
9 => 12
== method
0 => 7
1 => 8
send -> 'M'
3 => 9
== dbl
0 => 0
1 => 0
2 => 0
3 => 1
4 => -1
5 => 3
6 => 2
7 => -2
8 => 6
== cond2
0 => 'even0'
1 => 'odd1'
2 => 'even2'
3 => 'odd3'
4 => 'even4'
5 => 5
int(10)
string(5) "1/0:a"
int(20)
string(5) "2/0:b"
int(21)
bool(true)
