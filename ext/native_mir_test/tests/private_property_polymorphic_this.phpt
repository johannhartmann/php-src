--TEST--
Cached property reads into CVs with a polymorphic $this
--DESCRIPTION--
A cached declared property read writes its result straight into a CV
whose old value needs no release; a counted old value, a polymorphic
$this with private properties, a subclass property of the same name, a
closure bound to an unrelated object and an unset property with __get
keep the general path and its semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Base { private $at = 1; private $html = 'base';
    function read() { $was = 'old' . $this->at; $was = $this->at; return $was . ':' . $this->html . ':' . ($this->at ?? 'n'); }
    function reader() { return function () { return $this->at ?? 'none'; }; }
    function drop() { unset($this->at); } }
class Child extends Base { public $at = 'child'; }
class Other extends Base {}
class Magic extends Base { function __get($n) { return "magic-$n"; } }
class Unrelated { public $at = 'unrelated'; }
$objs = [new Base, new Child, new Other, new Base, new Other];
for ($r = 0; $r < 3; $r++) {
    $out = [];
    foreach ($objs as $o) { $out[] = $o->read(); }
    echo implode(' ', $out), "\n";
}
$f = (new Base)->reader();
echo Closure::bind($f, new Unrelated, Base::class)(), ' ', $f(), "\n";
$m = new Magic; $m->drop();
echo @$m->read(), "\n";
?>
--EXPECT--
1:base:1 1:base:1 1:base:1 1:base:1 1:base:1
1:base:1 1:base:1 1:base:1 1:base:1 1:base:1
1:base:1 1:base:1 1:base:1 1:base:1 1:base:1
unrelated 1
magic-at:base:magic-at
