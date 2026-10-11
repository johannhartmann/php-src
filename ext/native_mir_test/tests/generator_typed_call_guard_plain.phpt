--TEST--
Native generators yield guarded direct call results (without opcache)
--DESCRIPTION--
A generator whose resume block ends in a typed-call guard appends the reloaded values as liveness operands after the guard's own; the guard reads only its own operands, so generators yielding direct call results compile and run like the VM.
--INI--
opcache.enable=0
opcache.enable_cli=0
--FILE--
<?php
function g($x) { return $x; }
function gi(int $x): int { return $x * 2; }
function gs(string $s): string { return "<$s>"; }
function y1($v) { yield g($v); }
function y2($v) { yield g($v); yield gi($v); yield g($v) . "!"; }
function y4($v) { $k = 0; while ($k < 3) { yield $k => gi($v + $k); $k++; } return g($v); }
function y5($v) { yield from y2($v); yield gs((string) $v); }
function y6($a, $b) { $acc = g($a); foreach ([1, 2] as $x) { $acc = gi($acc) + g($x); yield $acc; } yield g($b); }
foreach (y1(3) as $x) var_dump($x);
foreach (y2(4) as $x) var_dump($x);
$g4 = y4(5); foreach ($g4 as $k => $x) echo "$k=$x "; var_dump($g4->getReturn());
foreach (y5(6) as $x) var_dump($x);
foreach (y6(1, "end") as $x) var_dump($x);
try { foreach (y2("x") as $x) var_dump($x); } catch (TypeError $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
?>
--EXPECTF--
int(3)
int(4)
int(8)
string(2) "4!"
0=10 1=12 2=14 int(5)
int(6)
int(12)
string(2) "6!"
string(3) "<6>"
int(3)
int(8)
string(3) "end"
string(1) "x"
TypeError: gi(): Argument #1 ($x) must be of type int, string given, called in %s on line 6
