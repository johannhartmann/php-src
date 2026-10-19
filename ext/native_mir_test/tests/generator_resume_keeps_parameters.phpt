--TEST--
Native generator resumes keep the parameters the body changed
--DESCRIPTION--
A generator's frame received its arguments before GENERATOR_CREATE
copied it. A resume continues at the saved position: it neither copies
defaults over parameters again nor re-verifies parameter types nor
rebuilds a variadic array.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function g1($n, $s = 1) { $s = 5; for ($i = 0; $i < $n; $i++) { yield $s; $s++; } }
foreach (g1(3) as $v) echo "$v,"; echo "\n";
function g2(int $n) { $n = "x"; yield 1; yield 2; echo $n, "\n"; }
foreach (g2(3) as $v) echo "$v,"; echo "\n";
function g3($a = [1]) { $a[] = 2; yield count($a); $a[] = 3; yield count($a); }
foreach (g3() as $v) echo "$v,"; echo "\n";
function g4(string ...$rest) { $rest[] = 9; yield count($rest); $rest = null; yield gettype($rest); }
foreach (g4("a", "b") as $v) echo "$v,"; echo "\n";
function g5($a) { $extra = func_get_args(); $a = 0; yield count($extra); yield $a; yield func_num_args(); }
foreach (g5(1, 2, 3) as $v) echo "$v,"; echo "\n";
class C {
    function m(float $f = 1.5, ?array $o = null) { $f = "s"; $o = [1]; $x = yield $f; yield $x . count($o); yield $f; }
}
$g = (new C)->m();
echo $g->current(), " ", $g->send("sent"), " ", ($g->next() ?? $g->current()), "\n";
function inner(int $k = 3) { $k = "inner"; yield 1; yield $k; }
function outer(int $j = 7) { $j = "outer"; yield from inner(); yield $j; }
foreach (outer() as $v) echo "$v,"; echo "\n";
function g6(int $n = 2) { for ($i = 0; $i < $n; $i++) { $n = $n + 1; if ($i > 3) { return $i; } yield $i; } }
$g = g6(); foreach ($g as $v) echo "$v,"; echo $g->getReturn(), "\n";
?>
--EXPECT--
5,6,7,
1,2,x

2,3,
3,NULL,
3,0,3,
s sent1 s
1,inner,outer,
0,1,2,3,4
