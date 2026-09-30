<?php
class D { public $n; function __construct($n) { $this->n = $n; } function __destruct() { echo "~{$this->n} "; } }
class Point {
    public $x = 1;
    function get($d) { return $this->x + $d; }
    function self_call($d) { return $this->get($d) * 2; }
    function defaults($a, $b = 10, $c = 'c') { return "$a/$b/$c"; }
    function typed(int $a): int { return $a * 3; }
    function bad_return($a): int { return $a; }
    function make($n) { return new D($n); }
    function thrower($a) { throw new RuntimeException("t$a"); }
}
class Other extends Point { function get($d) { return -$d; } }
function lib_add($a, $b) { return $a + $b; }
function lib_defaults($a, $b = [1, 2], $c = null) { return json_encode([$a, $b, $c]); }
function lib_typed(string $s, ?array $a = null): string { return $s . count($a ?? []); }
function lib_extra($a) { return count(func_get_args()) . ':' . $a; }
function lib_rec($n) { return $n <= 0 ? 0 : 1 + lib_rec($n - 1); }
function lib_make($n) { return new D($n); }
function lib_throw($a) { throw new LogicException("l$a"); }
function lib_ref_arg($a) { return $a; }
function lib_variadic($a, ...$r) { return $a . ':' . implode(',', $r); }
function lib_variadic_ref(&...$r) { foreach ($r as &$v) { $v++; } }
function lib_sq(int $a): int { return $a * $a; }
function lib_gen($obj) { return $obj->get(lib_add(yield, 1)); }
function lib_typed_defaults(int $a, float $f = 1.5, ?string $s = null, array $l = [1, 2], bool $b = true) { return json_encode([$a, $f, $s, $l, $b]); }
function lib_coerce(float $f, int $i = 3) { return var_export($f, true) . "/$i"; }
function lib_const_default($a = PHP_INT_SIZE, $b = D::class) { return "$a$b"; }
function lib_obj(Point $p, $d = 1) { return $p->get($d); }
