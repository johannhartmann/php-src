--TEST--
Native inline property reads into temporaries
--DESCRIPTION--
A cached declared-property read whose result has no machine value, such as
one consumed by a call, a concatenation or an array literal, copies the
property into its temporary inline as the helper's cached read does.
References, magic __get, dynamic and uninitialized typed properties and
other classes take the helper.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Base { public $name = 'base'; public $list = [1, 2]; public $ref; public int $typed; public $obj;
    function __construct() { $this->obj = new ArrayObject([1]); $x = 'r'; $this->ref = &$x; }
    function describe() { return strtoupper($this->name) . ':' . count($this->list) . ':' . $this->ref . ':' . get_class($this->obj); }
    function pass() { return implode(',', $this->list) . str_repeat($this->name, 2); }
    function typed() { try { return 'T' . $this->typed; } catch (Error $e) { return get_class($e); } }
    function arr() { return [$this->name => $this->list, 'o' => $this->obj]; }
}
class Child extends Base { public $name = 'child'; }
class Magic { private $data = ['m' => 'magic']; function __get($n) { return $this->data[$n] ?? 'none'; } function read() { return 'v=' . $this->m; } }
#[AllowDynamicProperties]
class Dyn { public $fixed = 'f'; }
function read_dynamic($o) { return $o->fixed . $o->extra; }
for ($i = 0; $i < 3; $i++) {
    foreach ([new Base, new Child] as $o) {
        $o->name .= $i;
        echo $o->describe(), ' ', $o->pass(), ' ', $o->typed(), ' ', json_encode(array_keys($o->arr())), "\n";
    }
    $d = new Dyn; $d->extra = "x$i";
    echo (new Magic)->read(), ' ', read_dynamic($d), "\n";
}
?>
--EXPECT--
BASE0:2:r:ArrayObject 1,2base0base0 Error ["base0","o"]
CHILD0:2:r:ArrayObject 1,2child0child0 Error ["child0","o"]
v=magic fx0
BASE1:2:r:ArrayObject 1,2base1base1 Error ["base1","o"]
CHILD1:2:r:ArrayObject 1,2child1child1 Error ["child1","o"]
v=magic fx1
BASE2:2:r:ArrayObject 1,2base2base2 Error ["base2","o"]
CHILD2:2:r:ArrayObject 1,2child2child2 Error ["child2","o"]
v=magic fx2
