--TEST--
Native: property reads of another class than the site cached
--DESCRIPTION--
A property read whose cache slot names another class reads through the
object's handler: subclasses, __get(), a throwing __get(), get hooks,
undefined properties and ?? reads alternate at the same sites.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
namespace NativePropertyOther;

class Base {
    public $v = 'base';
    function get() { return $this->v; }
    function getOr() { return $this->v ?? 'none'; }
}
class Child extends Base { public $extra = 2; public $v = 'child'; }
class Magic extends Base {
    function __construct() { unset($this->v); }
    function __get($name) { return "magic:$name"; }
}
class Thrower extends Base {
    function __construct() { unset($this->v); }
    function __get($name) { throw new \RuntimeException("no $name"); }
}
class Hooked { public $v { get => 'hooked'; } }
#[\AllowDynamicProperties]
class Dynamic {}
class Plain {}

function read(object $o) { return $o->v; }
function readOr(object $o) { return $o->v ?? 'null'; }

$dynamic = new Dynamic;
$dynamic->v = 'dynamic';
$objects = [new Base, new Child, new Magic, new Hooked, $dynamic, new Base];
for ($round = 0; $round < 3; $round++) {
    $out = [];
    foreach ($objects as $object) {
        $out[] = read($object) . '/' . readOr($object);
        if ($object instanceof Base) {
            $out[] = $object->get() . '/' . $object->getOr();
        }
    }
    echo implode(' ', $out), "\n";
}
try {
    read(new Thrower);
} catch (\RuntimeException $e) {
    echo $e->getMessage(), ' at line ', $e->getLine(), "\n";
}
try {
    (new Thrower)->get();
} catch (\RuntimeException $e) {
    echo $e->getMessage(), ' at line ', $e->getLine(), "\n";
}
var_dump(read(new Plain));
var_dump(readOr(new Plain));
?>
--EXPECTF--
base/base base/base child/child child/child magic:v/magic:v magic:v/magic:v hooked/hooked dynamic/dynamic base/base base/base
base/base base/base child/child child/child magic:v/magic:v magic:v/magic:v hooked/hooked dynamic/dynamic base/base base/base
base/base base/base child/child child/child magic:v/magic:v magic:v/magic:v hooked/hooked dynamic/dynamic base/base base/base
no v at line 16
no v at line 16

Warning: Undefined property: NativePropertyOther\Plain::$v in %s on line 23
NULL
string(4) "null"
