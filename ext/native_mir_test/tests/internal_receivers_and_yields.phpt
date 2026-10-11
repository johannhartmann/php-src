--TEST--
Native internal method receivers and generator operands
--DESCRIPTION--
Temporary and referenced receivers of internal methods, static calls through called scopes, and yield values and keys that are undefined, referenced or temporary keep stock ownership and diagnostics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D { public $n; function __construct($n) { $this->n = $n; } function __destruct() { echo "~{$this->n}|"; } }
class MyAO extends ArrayObject {
    public function __construct($a) { parent::__construct($a); echo "ctor|"; }
    public static function make($a) { return new static($a); }
    public function cnt() { return parent::count(); }
}
class MyDT extends DateTimeImmutable {
    public static function at($s) { return static::createFromFormat('Y-m-d', $s); }
}
function obj($a) { return new ArrayObject($a); }
function receivers($i) {
    $r = [];
    $r[] = (new ArrayObject([1, 2, $i]))->count();
    $r[] = obj([1])->count();
    $r[] = obj([new D("x$i")])->count();
    echo "[after x]|";
    $o = new ArrayObject([1, 2]);
    $ref = &$o;
    $r[] = $ref->count();
    $r[] = $o->count();
    $arr = ['o' => new ArrayObject([5])];
    $r[] = $arr['o']->count();
    $r[] = MyAO::make([1, 2, 3])->cnt();
    $r[] = MyDT::at("2020-01-0" . ($i + 1))->format('Y-m-d');
    $r[] = get_class(MyDT::at("2021-02-03"));
    try { $n = null; $n->count(); } catch (Error $e) { $r[] = $e->getMessage(); }
    $gens = (function () { yield 1; yield 2; })();
    $r[] = iterator_to_array($gens);
    return $r;
}
function &refgen(array &$a) { foreach ($a as $k => &$v) { yield $k => $v; } }
function gens($i) {
    $out = [];
    $g = (function () { $x = yield $undef_value; $out = yield $undef_key => 'v'; yield 'k' => $x; return 5; })();
    foreach ($g as $k => $v) { $out[] = [$k, $v]; }
    $out[] = $g->getReturn();
    $a = [1, 2];
    foreach (refgen($a) as &$v) { $v *= 10; }
    unset($v);
    $out[] = $a;
    $inner = (function () { yield 1; yield 2; return 'ret'; })();
    $outer = (function () use ($inner) { $r = yield from $inner; yield $r; yield from [7, 8]; yield from new ArrayIterator([9]); })();
    foreach ($outer as $k => $v) { $out[] = "$k:$v"; }
    $ref = 'refkey';
    $rk = &$ref;
    $g2 = (function () use (&$rk) { yield $rk => $rk; yield new D("k$GLOBALS[i]") => 1; })();
    foreach ($g2 as $k => $v) { $out[] = is_object($k) ? 'objkey' : $k; }
    return $out;
}
for ($i = 0; $i < 2; $i++) {
    echo json_encode(receivers($i)), "\n";
    echo json_encode(gens($i)), "\n";
}
--EXPECTF--
~x0|[after x]|ctor|[3,1,1,2,2,1,3,"2020-01-01","MyDT","Call to a member function count() on null",[1,2]]

Warning: Undefined variable $undef_value in %s on line 35

Warning: Undefined variable $undef_key in %s on line 35
~k0|[[0,null],[null,"v"],["k",null],5,[10,20],"0:1","1:2","0:ret","0:7","1:8","0:9","refkey","objkey"]
~x1|[after x]|ctor|[3,1,1,2,2,1,3,"2020-01-02","MyDT","Call to a member function count() on null",[1,2]]

Warning: Undefined variable $undef_value in %s on line 35

Warning: Undefined variable $undef_key in %s on line 35
~k1|[[0,null],[null,"v"],["k",null],5,[10,20],"0:1","1:2","0:ret","0:7","1:8","0:9","refkey","objkey"]
