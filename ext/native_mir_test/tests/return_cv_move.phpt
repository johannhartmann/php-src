--TEST--
Native return of a CV moves its value as the VM does
--DESCRIPTION--
A function returning a CV moves a counted value into the return value and
nulls the CV; a value that may leak is buffered as a possible GC root at
the return. GC roots, destructor order and the returned values match the VM.
--EXTENSIONS--
native_mir_test
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D {
    public $self;
    function __construct(public $n) {}
    function __destruct() { echo " ~{$this->n}"; }
}
function str($n) { $s = str_repeat('x', $n); return $s; }
function arr($n) { $a = [$n, $n + 1]; return $a; }
function obj($n) { $o = new D("o$n"); $t = new D("t$n"); return $o; }
function param($p) { return $p; }
function cyc($n) { $o = new D("c$n"); $o->self = $o; return $o; }
function keep($x) { static $kept = []; $kept[] = $x; return $x; }
$roots = [];
for ($i = 0; $i < 40; $i++) {
    $s = str($i % 4);
    $a = arr($i);
    $p = param($a);
    $k = keep([$i]);
    if ($i % 10 === 0) {
        $o = obj($i);
        echo " |";
        $o = null;
        echo "\n";
        $c = cyc($i);
        $c = null;
        $roots[] = gc_status()['roots'];
    }
}
echo strlen($s), " ", count($a), " ", $p[0], " ", $k[0], "\n";
echo implode(",", $roots), "\n";
gc_collect_cycles();
echo "\ndone\n";
?>
--EXPECT--
 ~t0 | ~o0
 ~t10 | ~o10
 ~t20 | ~o20
 ~t30 | ~o30
3 2 39 39
4,15,26,37
 ~c30 ~c20 ~c10 ~c0
done
