<?php
function dyn_add($a, $b = 10) { return $a + $b; }
function dyn_ref(&$a) { $a++; return $a; }
function dyn_typed(int $a): int { return $a * 2; }
function dyn_variadic($a, ...$r) { return $a . ':' . implode(',', $r); }
function dyn_named($a = 1, $b = 2, $c = 3) { return "$a$b$c"; }
function dyn_throw($a) { throw new LogicException("thrown $a"); }
function dyn_gen($a) { yield $a; yield $a + 1; }
function dyn_rec($f, $n) { return $n <= 0 ? 0 : 1 + call_user_func_array($f, [$f, $n - 1]); }
class DynBase {
    public $v = 5;
    function get($d) { return $this->v + $d; }
    private function secret($d) { return "secret$d"; }
    function callSecret($d) { return call_user_func_array([$this, 'secret'], [$d]); }
    static function who() { return static::class; }
    static function callWho() { return call_user_func(['DynBase', 'who']); }
    function callWhoFromInstance() { return call_user_func_array(['DynBase', 'who'], []); }
}
class DynChild extends DynBase { public $v = 7; }
class DynDtor { function __destruct() { echo "~dtor "; } function m($x) { return "m$x"; } }
