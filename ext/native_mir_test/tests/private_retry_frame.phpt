--TEST--
Read-only callees run in a private frame and retry through a Zend frame
--FILE--
<?php
final class Registry
{
    private $types = ['a' => 1, 'core/paragraph' => 2, 7 => 3];
    public $dynamicMissing;

    public function is_registered($name) { return isset($name) && isset($this->types[$name]); }
    public function get($name) { return $this->is_registered($name) ? $this->types[$name] : null; }
    public function first($name) { return $this->types[$name]; }
    public function readFirst($name) { return $this->first($name); }
    public function missing() { return $this->undeclared; }
    public function readMissing() { return $this->missing(); }
}

final class Magic
{
    public function __isset($name) { echo "__isset($name)\n"; return $name === 'items'; }
    public function __get($name) { echo "__get($name)\n"; return ['k' => 'v']; }
    public function has($key) { return isset($this->items[$key]); }
    public function hasHere($key) { return $this->has($key); }
}

final class Throwing
{
    public function __isset($name) { throw new RuntimeException("no $name"); }
    public function has($key) { return isset($this->items[$key]); }
    public function hasHere($key) { return $this->has($key); }
}

class Offsets implements ArrayAccess
{
    public function offsetExists($offset): bool { echo "offsetExists($offset)\n"; return $offset === 'x'; }
    public function offsetGet($offset): mixed { echo "offsetGet($offset)\n"; return 'got'; }
    public function offsetSet($offset, $value): void {}
    public function offsetUnset($offset): void {}
}

final class Holder
{
    public $plain = ['x' => 'plain-value'];
    public $offsets;
    public function __construct() { $this->offsets = new Offsets(); $this->plain['y'] = str_repeat('p', 3); }
    public function same($a, $b) { return $this->plain[$a] === $this->offsets[$b]; }
    public function compare($a, $b) { return $this->same($a, $b); }
    public function isset2($k) { return isset($this->offsets[$k]); }
    public function viaIsset($k) { return $this->isset2($k); }
}

final class Typed
{
    public function id(int $n) { return $n === 3; }
    public function call($v) { return $this->id($v); }
}

final class Scope
{
    protected static function valid($v) { return '' !== $v && isset($v); }
    public static function check($v) { return static::valid($v); }
}

$r = new Registry();
for ($i = 0; $i < 3; $i++) {
    var_dump($r->get('a'), $r->get('core/paragraph'), $r->get('nope'), $r->get(7), $r->get(null));
}
for ($i = 0; $i < 2; $i++) {
    var_dump($r->readFirst('a'));
    var_dump($r->readFirst('zzz'));
    var_dump($r->readMissing());
}

$alias = 'core/paragraph';
$ref = &$alias;
var_dump($r->get($alias));
var_dump($r->get($undefinedVariable));

$m = new Magic();
for ($i = 0; $i < 2; $i++) {
    var_dump($m->hasHere('k'), $m->hasHere('z'));
}

$t = new Throwing();
for ($i = 0; $i < 2; $i++) {
    try {
        $t->hasHere('k');
    } catch (RuntimeException $e) {
        echo get_class($e), ': ', $e->getMessage(), ' at line ', $e->getLine(), "\n";
    }
}

$h = new Holder();
for ($i = 0; $i < 2; $i++) {
    var_dump($h->compare('x', 'y'));
    var_dump($h->compare('y', 'x'));
    var_dump($h->viaIsset('x'), $h->viaIsset('q'));
}

$ty = new Typed();
for ($i = 0; $i < 2; $i++) {
    var_dump($ty->call(3), $ty->call('3'), $ty->call(4));
    try {
        $ty->call('three');
    } catch (TypeError $e) {
        echo get_class($e), ' at line ', $e->getLine(), "\n";
    }
}

for ($i = 0; $i < 2; $i++) {
    var_dump(Scope::check('x'), Scope::check(''), Scope::check(null));
}

function ident($v) { return $v; }
function identities($s, $n) {
    $a = $s . 'x';
    $b = 'lit';
    $c = [$n, $s];
    $o = new ArrayObject([$n]);
    $c2 = ident($c);
    $c2[] = 'changed';
    return [ident($a), ident($b), ident($n), $c, $c2, ident($o) === $o, ident(ident($a)) . ident($b)];
}
for ($i = 0; $i < 3; $i++) {
    echo json_encode(identities("s$i", $i)), "\n";
}

function counted() {
    $r = new Registry();
    $n = 0;
    for ($i = 0; $i < 100; $i++) {
        if ($r->is_registered('core/paragraph')) {
            $n++;
        }
    }
    return $n;
}
var_dump(counted());
?>
--EXPECTF--
int(1)
int(2)
NULL
int(3)
NULL
int(1)
int(2)
NULL
int(3)
NULL
int(1)
int(2)
NULL
int(3)
NULL
int(1)

Warning: Undefined array key "zzz" in %s on line %d
NULL

Warning: Undefined property: Registry::$undeclared in %s on line %d
NULL
int(1)

Warning: Undefined array key "zzz" in %s on line %d
NULL

Warning: Undefined property: Registry::$undeclared in %s on line %d
NULL
int(2)

Warning: Undefined variable $undefinedVariable in %s on line %d
NULL
__isset(items)
__get(items)
__isset(items)
__get(items)
bool(true)
bool(false)
__isset(items)
__get(items)
__isset(items)
__get(items)
bool(true)
bool(false)
RuntimeException: no items at line 25
RuntimeException: no items at line 25
offsetGet(y)
bool(false)
offsetGet(x)
bool(false)
offsetExists(x)
offsetExists(q)
bool(true)
bool(false)
offsetGet(y)
bool(false)
offsetGet(x)
bool(false)
offsetExists(x)
offsetExists(q)
bool(true)
bool(false)
bool(true)
bool(true)
bool(false)
TypeError at line 51
bool(true)
bool(true)
bool(false)
TypeError at line 51
bool(true)
bool(false)
bool(false)
bool(true)
bool(false)
bool(false)
["s0x","lit",0,[0,"s0"],[0,"s0","changed"],true,"s0xlit"]
["s1x","lit",1,[1,"s1"],[1,"s1","changed"],true,"s1xlit"]
["s2x","lit",2,[2,"s2"],[2,"s2","changed"],true,"s2xlit"]
int(100)
