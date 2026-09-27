--TEST--
Native x64 nested direct user calls
--DESCRIPTION--
A group of overlapping calls, such as ack($m - 1, ack($m, $n - 1)), uses
direct calls that transfer their arguments at DO, and a temporary sent to
a call keeps its slot until that DO. Arguments already sent when an inner
call or operand throws are released, references and globals changed by an
inner call keep the SEND-time value, and evaluation order, destructors and
argument type errors keep VM behavior.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function ack($m, $n) {
    if ($m == 0) return $n + 1;
    if ($n == 0) return ack($m - 1, 1);
    return ack($m - 1, ack($m, $n - 1));
}
function pair($a, $b) { return "[$a,$b]"; }
function thrower($x) { if ($x > 1) throw new RuntimeException("t$x"); return $x; }
function nested_strings($a, $b) { return pair($a . "x", pair($b . "y", $a)); }
function nested_throw($a, $n) {
    try {
        return pair($a . "-kept", thrower($n));
    } catch (RuntimeException $e) {
        return "caught " . $e->getMessage();
    }
}
function deep_throw($a, $n) {
    try {
        return pair($a . "1", pair($a . "2", pair($a . "3", thrower($n))));
    } catch (RuntimeException $e) {
        return "deep " . $e->getMessage();
    }
}
function mid_throw($a, $z) {
    try {
        return pair($a . "!", $z % 0);
    } catch (DivisionByZeroError $e) {
        return "div";
    }
}
function by_ref(&$v) { $v = "changed"; return "r"; }
function ref_mutation() {
    $x = "orig";
    return pair($x, by_ref($x)) . $x;
}
function global_mutation() {
    global $g;
    $g = "orig";
    return pair($g, set_g());
}
function set_g() { global $g; $g = "changed"; return "s"; }
$counter = 0;
function tick() { global $counter; return ++$counter; }
function order() { return pair(tick(), pair(tick(), tick())) . pair(tick() * 10, tick() - 1); }
function pi_arg($m, $n) { if ($m == 0) return 1; return pair($m - 1, $m) . pair($m, $n); }
function objects($o) { return pair(get_class($o), pair(spl_object_id($o) > 0, count((array) $o))); }
class Dtor { public function __construct(public $n) {} public function __destruct() { echo "dtor{$this->n} "; } }
function mk($n) { return new Dtor($n); }
function take($a, $b) { return is_object($a) ? $a->n + $b : $b; }
function dtor_order($n) {
    try {
        return take(mk(1), thrower($n));
    } catch (RuntimeException $e) {
        return "dt " . $e->getMessage();
    }
}
function typed(int $a, string $b): string { return "$a:$b"; }
function typed_nested($x) {
    try {
        return typed($x + 1, typed($x, "q"));
    } catch (TypeError $e) {
        return "type error";
    }
}
for ($round = 0; $round < 3; $round++) {
    echo ack(2, 3), " ", ack(3, 2), "\n";
    echo nested_strings("a", "b"), "\n";
    echo nested_throw("s", 1), " ", nested_throw("s", 2), "\n";
    echo deep_throw("d", 1), " ", deep_throw("d", 5), "\n";
    echo mid_throw("m", 3), "\n";
    echo ref_mutation(), "\n";
    echo global_mutation(), "\n";
    $counter = 0; echo order(), "\n";
    echo pi_arg(0, 1), " ", pi_arg(3, 4), " ", pi_arg(2.5, "x"), "\n";
    echo objects(new stdClass), "\n";
    echo dtor_order(1), "\n"; echo dtor_order(3), "\n";
    echo typed_nested(1), " ", typed_nested(1.5), "\n";
}
?>
--EXPECTF--
9 29
[ax,[by,a]]
[s-kept,1] caught t2
[d1,[d2,[d3,1]]] deep t5
div
[orig,r]changed
[orig,s]
[1,[2,3]][40,4]
1 [2,3][3,4] [1.5,2.5][2.5,x]
[stdClass,[1,0]]
dtor1 2
dtor1 dt t3
2:1:q 
Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 57

Deprecated: Implicit conversion from float 2.5 to int loses precision in %s on line 57
2:1:q
9 29
[ax,[by,a]]
[s-kept,1] caught t2
[d1,[d2,[d3,1]]] deep t5
div
[orig,r]changed
[orig,s]
[1,[2,3]][40,4]
1 [2,3][3,4] [1.5,2.5][2.5,x]
[stdClass,[1,0]]
dtor1 2
dtor1 dt t3
2:1:q 
Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 57

Deprecated: Implicit conversion from float 2.5 to int loses precision in %s on line 57
2:1:q
9 29
[ax,[by,a]]
[s-kept,1] caught t2
[d1,[d2,[d3,1]]] deep t5
div
[orig,r]changed
[orig,s]
[1,[2,3]][40,4]
1 [2,3][3,4] [1.5,2.5][2.5,x]
[stdClass,[1,0]]
dtor1 2
dtor1 dt t3
2:1:q 
Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 57

Deprecated: Implicit conversion from float 2.5 to int loses precision in %s on line 57
2:1:q
