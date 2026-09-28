--TEST--
Native x64 inline call frames move call results sent as arguments
--DESCRIPTION--
The result of a call to a function that does not return by reference moves
into an inline callee frame like a temporary, as in Ack($m - 1, Ack(...)).
Results of by-reference functions, strings, arrays and exceptions thrown by
the inner call keep their VM behavior.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function Ack($m, $n) { if ($m == 0) return $n + 1; if ($n == 0) return Ack($m - 1, 1); return Ack($m - 1, Ack($m, $n - 1)); }
function wrap($x) { return "<$x>"; }
function join2($a, $b) { return $a . $b; }
function &counter() { static $v = 5; return $v; }
function add($a, $b) { return $a + $b; }
function pair($n) { return [$n, $n * 2]; }
function first(array $a, $b) { return $a[0] + $b; }
function fail($n) { if ($n > 1) throw new RuntimeException("fail $n"); return $n; }
function keep($a, $b) { return "$a/$b"; }
class D { public function __destruct() { echo "destruct\n"; } }
function make() { return new D; }
function drop($a, $b) { return $b; }
for ($i = 0; $i < 3; $i++) {
    echo Ack(2, 3), " ", join2(wrap("a"), wrap("b")), " ", add(counter(), counter()), " ", first(pair($i), first(pair(1), 1)), "\n";
    try {
        echo keep(wrap("x"), fail($i)), "\n";
    } catch (RuntimeException $e) {
        echo $e->getMessage(), "\n";
    }
    echo drop(make(), 7), "\n";
}
?>
--EXPECT--
9 <a><b> 10 2
<x>/0
destruct
7
9 <a><b> 10 3
<x>/1
destruct
7
9 <a><b> 10 4
fail 2
destruct
7
