--TEST--
Native call entry releases $this, closures and extra arguments on leave
--EXTENSIONS--
native_mir_test
--FILE--
<?php
class D { public function __construct(public string $n) {} public function __destruct() { echo "[d:{$this->n}]"; } }
class T { public function __destruct() { echo "[t]"; } public function m($a = 1) { return "m$a"; } }
class Bad { public function __destruct() { throw new Exception('dtor'); } public function m() { return 'bad'; } }
function extra_args() { return func_num_args(); }
function variadic_hold(...$a) { return count($a); }
function make() { return new T; }
for ($round = 0; $round < 3; $round++) {
    echo make()->m(), "\n";
    $t = new T; echo $t->m(2), ' '; unset($t); echo "\n";
    $f = function ($x) { return "c$x"; };
    echo $f(1), ' ', call_user_func([new T, 'm'], 3), "\n";
    unset($f);
    echo extra_args(new D("e$round"), new D("f$round")), "\n";
    echo variadic_hold(new D("v$round"), 1), "\n";
    try { echo (new Bad)->m(), "\n"; } catch (Exception $e) { echo 'caught ', $e->getMessage(), "\n"; }
    $o = new T; $cb = [$o, 'm']; unset($o); echo call_user_func($cb), ' '; unset($cb); echo "\n";
}
echo gc_collect_cycles() >= 0 ? "gc ok\n" : '';
class Bad2 { public function __destruct() { throw new Exception('dtor2'); } public function m() { $s = ''; for ($i = 0; $i < 2; $i++) { $s .= 'b'; } return $s; } }
function consume() { try { echo strlen((new Bad2)->m()), "\n"; } catch (Exception $e) { echo 'caught ', $e->getMessage(), "\n"; } }
function consume2() { try { $x = (new Bad2)->m(); echo "x=$x\n"; } catch (Exception $e) { echo 'caught ', $e->getMessage(), "\n"; } }
$cl = function () { return str_repeat('c', 3); };
for ($round = 0; $round < 3; $round++) { consume(); consume2(); echo $cl(), "\n"; }
--EXPECTF--
[t]m1
m2 [t]
c1 [t]m3
[d:e0][d:f0]2
[d:v0]2
caught dtor
m1 [t]
[t]m1
m2 [t]
c1 [t]m3
[d:e1][d:f1]2
[d:v1]2
caught dtor
m1 [t]
[t]m1
m2 [t]
c1 [t]m3
[d:e2][d:f2]2
[d:v2]2
caught dtor
m1 [t]
gc ok
caught dtor2
caught dtor2
ccc
caught dtor2
caught dtor2
ccc
caught dtor2
caught dtor2
ccc
