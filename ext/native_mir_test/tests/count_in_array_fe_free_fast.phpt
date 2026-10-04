--TEST--
count(), in_array() of a constant array and FE_FREE without decoding
--DESCRIPTION--
count() of an array, in_array() of a string needle in a constant array
and the release of an iterated array take decode-free paths; Countable
objects, invalid arguments, non-string needles, strict mode and
destructors of released elements keep their semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D { function __construct(public $n) {} function __destruct() { echo "~{$this->n} "; } }
class C implements Countable { function count(): int { return 42; } }
function run($k, $s) {
    $a = range(0, $k); $r = &$a; $n = 7;
    $out = [count($a), count($r), count(array_fill(0, $k + 2, 0)), count(new C)];
    $n = count($a);
    $out[] = $n;
    $out[] = in_array($s, ['alpha', 'beta', '1']);
    $out[] = in_array($s . 'x', ['alpha', 'betax']);
    $out[] = in_array(1, ['alpha', '1']);
    $out[] = in_array('1', ['alpha', '1'], true);
    foreach ([new D("t$k"), new D("u$k")] as $d) { $out[] = $d->n; }
    unset($d);
    foreach (array_map(fn($x) => $x * 2, $a) as $v) { $out[] = $v; }
    return json_encode($out);
}
foreach (['alpha', 'beta', 'gamma'] as $i => $s) { echo run($i, $s), "\n"; }
try { count(null); } catch (TypeError $e) { echo get_class($e), "\n"; }
?>
--EXPECT--
~t0 ~u0 [1,1,2,42,1,true,false,true,true,"t0","u0",0]
~t1 ~u1 [2,2,3,42,2,true,true,true,true,"t1","u1",0,2]
~t2 ~u2 [3,3,4,42,3,false,false,true,true,"t2","u2",0,2,4]
TypeError
