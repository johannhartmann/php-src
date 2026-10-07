--TEST--
Dynamic calls send inline: call_user_func_array() with packed, referenced, holed and named arrays, call_user_func() and $f() positional sends, closures released in the leave
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function plain($a, $b = 'd') { return "$a/$b"; }
function byref(&$a) { $a .= '!'; return $a; }
function vari(...$xs) { return implode('+', $xs); }
function many(...$xs) { return count($xs); }
class O { function m($x) { return "m$x"; } }
function cufa($f, $args) { return call_user_func_array($f, $args); }
$o = new O;
$s = 'str';
$r = [];
for ($i = 0; $i < 30; $i++) {
    $r[] = cufa('plain', [$i, 'x']);
    $r[] = cufa('plain', [$i]);
    $r[] = cufa([$o, 'm'], [$i]);
    $r[] = cufa(fn($a) => $a * 2, [$i]);
    $r[] = cufa('vari', [1, 2, $i]);
    $arr = [$s, &$s];
    $r[] = cufa('plain', $arr);
    $holes = [0 => 'a', 2 => 'c'];
    unset($holes[2]); $holes[] = 'z';
    $r[] = cufa('plain', $holes);
    $r[] = cufa('many', range(1, 20));
    $r[] = cufa('plain', ['a' => 'named', 'b' => 'B']);
}
$v = 'ref';
$r[] = call_user_func_array('byref', [&$v]);
$r[] = $v;
echo md5(implode(',', $r)), "\n", implode(',', array_slice($r, 0, 12)), "\n";
try { cufa('plain', []); } catch (ArgumentCountError $e) { echo get_class($e), ': ', $e->getMessage(), "\n"; }

function two_d($a, $b = 'd') { return "$a/$b"; }
function byref_d(&$a) { $a .= '!'; return $a; }
function many_d($a, $b, $c, $d, $e, $f, $g, $h, $i, $j, $k, $l, $m, $n) { return $a + $n; }
class O_d { function m($x, $y = 2) { return "m$x$y"; } }
$o = new O_d;
$fs = ['two_d', fn($a, $b = 'c') => "$a+$b", [$o, 'm']];
$r = [];
for ($i = 0; $i < 40; $i++) {
    foreach ($fs as $f) {
        $s = "s$i";
        $r[] = $f($s);
        $r[] = $f($i, 1.5);
        $r[] = $f(strtoupper($s), null);
        $r[] = call_user_func($f, $s, true);
        $x = [$i]; $ref = &$x[0];
        $r[] = $f($ref);
    }
    $r[] = many_d(...range(1, 14));
}
$v = 'v';
$g = 'byref_d';
$r[] = $g($v); $r[] = $v;
echo md5(implode(',', $r)), "\n", implode(',', array_slice($r, 0, 16)), "\n";
$u = 'two_d';
try { $u(); } catch (ArgumentCountError $e) { echo $e->getMessage(), "\n"; }
echo $u($undefined_var), "\n";
function refp(&$a) { $a = 'changed'; return 'r'; }
$w = 'orig';
echo call_user_func('refp', $w), $w, "\n";
echo call_user_func('two_d', $undefined2 ?? 'dflt'), "\n";

class D { public $n; function __construct($n) { $this->n = $n; } function __destruct() { echo "destruct {$this->n}\n"; } }
class B { public $v = 'b'; function get() { return function ($x) { return $this->v . $x; }; } function m($x) { return "m$x"; } }
function plain_e($x) { return "p$x"; }
$b = new B;
$fs = [function ($x) { return "c$x"; }, static fn($x) => "s$x", $b->get(), plain_e(...), $b->m(...), Closure::fromCallable('plain_e')];
$r = [];
for ($i = 0; $i < 30; $i++) { foreach ($fs as $f) { $r[] = $f($i); $r[] = call_user_func($f, $i); $r[] = call_user_func_array($f, [$i]); } }
echo md5(implode(',', $r)), "\n", implode(',', array_slice($r, 0, 18)), "\n";
for ($i = 0; $i < 3; $i++) { $d = new D($i); echo (function () use ($d) { return "t{$d->n}"; })(), "\n"; unset($d); echo "after $i\n"; }
$k = (function () { $d = new D('in'); return fn() => $d->n; })();
echo $k(), "\n"; unset($k); echo "end\n";
--EXPECTF--
85da234259f7b7f722d990639fa11b33
0/x,0/d,m0,0,1+2+0,str/str,a/z,20,named/B,1/x,1/d,m1
ArgumentCountError: Too few arguments to function plain(), 0 passed in %s on line 7 and at least 1 expected
da6b2a127057db12d61b79b2772ec9e7
s0/d,0/1.5,S0/,s0/1,0/d,s0+c,0+1.5,S0+,s0+1,0+c,ms02,m01.5,mS0,ms01,m02,15
Too few arguments to function two_d(), 0 passed in %s on line 55 and at least 1 expected

Warning: Undefined variable $undefined_var in %s on line 56
/d

Warning: refp(): Argument #1 ($a) must be passed by reference, value given in %s on line 59
rorig
dflt/d
efd926aa6df9b8db7b657801e0b86627
c0,c0,c0,s0,s0,s0,b0,b0,b0,p0,p0,p0,m0,m0,m0,p0,p0,p0
t0
destruct 0
after 0
t1
destruct 1
after 1
t2
destruct 2
after 2
in
destruct in
end
