--TEST--
Native call-site fast path for new of a literal class
--DESCRIPTION--
new C(...) of a literal class whose site published its constructor creates
the object in the result slot and calls the constructor through the fast
frame, as ZEND_NEW does. Defaults, inherited constructors, classes without
a constructor, a throwing constructor, an exception while evaluating the
arguments, abstract classes and destructor order keep stock semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
require __DIR__ . '/w14_call_fast_new.inc';
function run($r) {
    $out = [];
    $p = new NPlain($r); $out[] = [$p->a, $p->b];
    $c = new NChild($r, 'x'); $out[] = [$c->a, $c->b, get_class($c)];
    $n = new NNoCtor; $out[] = $n->x;
    try { $t = new NThrow($r); $out[] = $t->n; } catch (RuntimeException $e) { $out[] = $e->getMessage(); }
    try { $q = new NPlain(nthrow_arg($r)); } catch (LogicException $e) { $out[] = $e->getMessage(); }
    try { $a = new NAbstract; } catch (Error $e) { $out[] = 'Abstract'; }
    new NDtor("tmp$r");
    echo "[after tmp] ";
    $d = new NDtor("kept$r");
    unset($d);
    return $out;
}
for ($i = 0; $i < 3; $i++) { echo json_encode(run($i)), "|\n"; }
?>
--EXPECT--
~NDtortmp0 [after tmp] ~NDtorkept0 ~NThrow0 [[0,"dflt"],[0,"x","NChild"],1,0,"arg0","Abstract"]|
~NDtortmp1 [after tmp] ~NDtorkept1 [[1,"dflt"],[1,"x","NChild"],1,"ctor1","arg1","Abstract"]|
~NDtortmp2 [after tmp] ~NDtorkept2 ~NThrow2 [[2,"dflt"],[2,"x","NChild"],1,2,"arg2","Abstract"]|
