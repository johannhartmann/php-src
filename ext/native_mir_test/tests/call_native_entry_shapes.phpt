--TEST--
Native call entries and direct calls of every argument and receiver shape
--DESCRIPTION--
The native call entry takes exact, defaulted, typed, nullable and variadic
parameter lists, functions without a return and closures; direct calls take
boolean and counted arguments, $this, temporary and chained receivers whose
release runs the destructor at the VM's point, and unused results.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function typed_args(int $a, string $b, float $c, ?int $d) {
    return "$a|$b|$c|" . var_export($d, true);
}
function defaults($a, $b = 2, int $c = 3) { return $a + $b + $c; }
function opt_typed(int $a, int $b = 5) { return $a * $b; }
function variadic_fn($a, ...$rest) { return $a . ':' . implode(',', $rest); }
function no_return($x) { $y = $x; }
function bool_arg(bool $flag, $v) { return $flag ? $v : -$v; }
function exact3($a, $b, $c) { return [$a, $b, $c]; }
function counted_locals($s) { $t = $s . 'x'; $arr = [$t, $s]; return count($arr) . $t; }

class Node {
    public $v;
    public function __construct($v) { $this->v = $v; }
    public function self() { return $this; }
    public function val() { return $this->v; }
    public function inc(int $by = 1) { $this->v += $by; return $this; }
    public function viaThis() { return $this->val() + $this->other(2); }
    private function other($x) { return $x * 10; }
    public static function make($v) { return new static($v); }
    public function __destruct() { echo "destruct {$this->v}\n"; }
}
final class F {
    public $v;
    public function __construct($v) { $this->v = $v; }
    public function me(): F { return $this; }
    public function get() { return $this->v; }
    private function priv($x) { return $x + $this->v; }
    public function usePriv($x) { return $this->priv($x); }
    public function __destruct() { echo "bye {$this->v}\n"; }
}
function make_f(int $v): F { return new F($v); }

function run() {
    $out = [];
    for ($i = 0; $i < 3; $i++) {
        $out[] = typed_args($i, "s$i", 1.5, $i % 2 ? null : $i);
        $out[] = defaults($i) . ' ' . defaults($i, 10) . ' ' . defaults($i, 10, 20);
        $out[] = opt_typed($i) . ' ' . opt_typed($i, 2);
        $out[] = variadic_fn($i) . ' ' . variadic_fn($i, 1, 2, 3);
        $out[] = var_export(no_return($i), true);
        no_return($i);
        $out[] = bool_arg($i > 0, $i) . ' ' . bool_arg(true, 7);
        $out[] = json_encode(exact3($i, "x", [1]));
        $out[] = counted_locals(str_repeat('a', $i + 1));
        $n = new Node($i);
        $out[] = $n->self()->val();
        $out[] = (new Node($i + 100))->val();
        $out[] = $n->inc()->inc(2)->val();
        $out[] = $n->viaThis();
        $out[] = Node::make($i + 200)->val();
        $c = function ($x) use ($i) { return $x + $i; };
        $out[] = $c(5);
        $out[] = (function ($x) use ($n) { return $x . $n->v; })('c');
        $out[] = call_user_func_array('exact3', [1, 2, 3])[2];
        $out[] = call_user_func_array([$n, 'val'], []);
        $f = 'defaults';
        $out[] = $f(1);
        $out[] = array_sum(array_map(fn($x) => $x * 2, [1, 2, $i]));
        $g = new F($i);
        $out[] = $g->me()->get() + (new F($i * 10))->get()
            + make_f($i + 100)->get() + $g->usePriv(5)
            + make_f($i + 200)->usePriv(1);
        unset($n, $g);
    }
    try {
        typed_args("x", "y", 1.0, null);
    } catch (TypeError $e) {
        $out[] = get_class($e);
    }
    try {
        opt_typed([]);
    } catch (Throwable $e) {
        $out[] = get_class($e);
    }
    $out[] = opt_typed("7");
    try {
        exact3(1, 2);
    } catch (ArgumentCountError $e) {
        $out[] = get_class($e);
    }
    return $out;
}
echo implode("\n", run()), "\n";
--EXPECT--
destruct 100
destruct 200
bye 0
bye 100
bye 200
destruct 3
bye 0
destruct 101
destruct 201
bye 10
bye 101
bye 201
destruct 4
bye 1
destruct 102
destruct 202
bye 20
bye 102
bye 202
destruct 5
bye 2
0|s0|1.5|0
5 13 30
0 0
0: 0:1,2,3
NULL
0 7
[0,"x",[1]]
2ax
0
100
3
23
200
5
c3
3
3
6
6
306
1|s1|1.5|NULL
6 14 31
5 2
1: 1:1,2,3
NULL
1 7
[1,"x",[1]]
2aax
1
101
4
24
201
6
c4
3
4
6
8
320
2|s2|1.5|2
7 15 32
10 4
2: 2:1,2,3
NULL
2 7
[2,"x",[1]]
2aaax
2
102
5
25
202
7
c5
3
5
6
10
334
TypeError
TypeError
35
ArgumentCountError
