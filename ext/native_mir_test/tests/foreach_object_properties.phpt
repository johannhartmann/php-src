--TEST--
Native foreach over an object's properties separates a shared table
--DESCRIPTION--
FE_RESET_RW separates a property table shared with an array cast before iterating by reference, and FE_RESET_R asks get_properties() only for an object without a table, once, as the VM does; lazy ghosts and proxies initialize first.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function iter($i) {
    $out = [];
    foreach ($i as $k => $v) {
        if ($k === 'd' || $k === 'y') $out[] = "$k=" . var_export($v, true);
    }
    return implode(',', $out);
}
function iter_ref($o) {
    foreach ($o as $k => &$v) {
        $v = is_int($v) ? $v * 10 : $v;
    }
    unset($v);
}
function run() {
    $i = new DateInterval('P1Y2D');
    echo iter($i), "\n";
    $i->d = 7;
    echo iter($i), "\n";
    $i->y = 3;
    echo iter($i), "\n";
    $o = new stdClass;
    $o->a = 1;
    $o->b = 2;
    $arr = (array) $o;
    iter_ref($o);
    var_dump($arr, $o);
    $e = new stdClass;
    foreach ($e as $v) { echo "never\n"; }
    foreach ($e as &$v) { echo "never\n"; }
    $p = new class { public $x = 1; private $y = 2; protected $z = 3; };
    foreach ($p as $k => $v) { echo "$k=$v\n"; }
    foreach ($p as $k => &$v) { $v++; }
    var_dump($p);
    $c = new stdClass;
    $c->q = 1;
    $copy = (array) $c;
    foreach ($c as $k => $v) { $c->r = 2; echo "$k\n"; }
    var_dump($copy, $c);
}
run();
run();
class L { public $a = 1; public $b = 2; }
function lazy() {
    $r = new ReflectionClass(L::class);
    $g = $r->newLazyGhost(function ($o) { echo "init ghost\n"; $o->a = 5; });
    foreach ($g as $k => $v) { echo "$k=$v\n"; }
    $p = $r->newLazyProxy(function ($o) { echo "init proxy\n"; $l = new L; $l->b = 9; return $l; });
    foreach ($p as $k => &$v) { $v = "r$v"; }
    unset($v);
    var_dump($p->a, $p->b);
    $p2 = $r->newLazyProxy(function ($o) { echo "init proxy2\n"; return new L; });
    $copy = (array) $p2;
    foreach ($p2 as $k => &$v) { $v = 0; }
    unset($v);
    var_dump($copy, $p2->a);
}
lazy();
lazy();
--EXPECT--
y=1,d=2
y=1,d=7
y=3,d=7
array(2) {
  ["a"]=>
  int(1)
  ["b"]=>
  int(2)
}
object(stdClass)#2 (2) {
  ["a"]=>
  int(10)
  ["b"]=>
  int(20)
}
x=1
object(class@anonymous)#4 (3) {
  ["x"]=>
  &int(2)
  ["y":"class@anonymous":private]=>
  int(2)
  ["z":protected]=>
  int(3)
}
q
r
array(1) {
  ["q"]=>
  int(1)
}
object(stdClass)#5 (2) {
  ["q"]=>
  int(1)
  ["r"]=>
  int(2)
}
y=1,d=2
y=1,d=7
y=3,d=7
array(2) {
  ["a"]=>
  int(1)
  ["b"]=>
  int(2)
}
object(stdClass)#4 (2) {
  ["a"]=>
  int(10)
  ["b"]=>
  int(20)
}
x=1
object(class@anonymous)#2 (3) {
  ["x"]=>
  &int(2)
  ["y":"class@anonymous":private]=>
  int(2)
  ["z":protected]=>
  int(3)
}
q
r
array(1) {
  ["q"]=>
  int(1)
}
object(stdClass)#1 (2) {
  ["q"]=>
  int(1)
  ["r"]=>
  int(2)
}
init ghost
a=5
b=2
init proxy
string(2) "r1"
string(2) "r9"
init proxy2
array(0) {
}
int(0)
init ghost
a=5
b=2
init proxy
string(2) "r1"
string(2) "r9"
init proxy2
array(0) {
}
int(0)
