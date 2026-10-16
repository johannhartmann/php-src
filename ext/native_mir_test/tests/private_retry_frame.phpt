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

final class Battery
{
    public $arr = ['x' => 'X', 3 => 'three', 'n' => null, 'z' => 0];
    public $str = 'hello';
    public $obj;
    public function __construct() { $this->obj = new stdClass(); }
    public function issetIf($k) { if (isset($this->arr[$k])) { return true; } return false; }
    public function issetAnd($k) { return isset($k) && isset($this->arr[$k]); }
    public function issetNot($k) { return !isset($this->arr[$k]); }
    public function emptyIf($k) { if (empty($this->arr[$k])) { return 'empty'; } return 'full'; }
    public function read($k) { return $this->arr[$k]; }
    public function readIs($k) { return $this->arr[$k] ?? 'dflt'; }
    public function readTern($k) { return isset($this->arr[$k]) ? $this->arr[$k] : 'none'; }
    public function strOffset($k) { return $this->str[$k]; }
    public function strIsset($k) { return isset($this->str[$k]); }
    public function objRead($k) { return $this->obj[$k]; }
    public function same($a, $b) { return $a === $b; }
    public function truthy($v) { if ($v) { return 'yes'; } return 'no'; }
    public function negate($v) { return !$v; }
    public function typed($v) { return is_string($v); }
    public function cvIsset($a, $k) { if (isset($a[$k])) { return 1; } return 0; }
    public function cvTern($a, $k) { return isset($a[$k]) ? $a[$k] : 'n'; }
    public function cvEmpty($a, $k) { if (empty($a[$k])) { return 'e'; } return 'f'; }
    public function cvSame($a, $k) { if ($a[$k] === 'X') { return 'same'; } return 'other'; }
    public function run() {
        $out = [];
        foreach (['x', 3, '3', 1.5, true, null, 'n', 'z', 'missing'] as $k) {
            $out[] = var_export([$this->issetIf($k), $this->issetAnd($k), $this->issetNot($k), $this->emptyIf($k), $this->readIs($k), $this->readTern($k)], true);
        }
        foreach (['x', 1.5, null, 'missing'] as $k) {
            $out[] = var_export($this->read($k), true);
        }
        $a = $this->arr;
        foreach (['x', 3, 1.5, true, null, 'missing'] as $k) {
            $out[] = var_export([$this->cvIsset($a, $k), $this->cvTern($a, $k), $this->cvEmpty($a, $k), $this->cvSame($a, $k)], true);
        }
        foreach (['x', 1.5, null] as $k) {
            $out[] = var_export([$this->cvIsset('str', $k), $this->cvIsset(null, $k), $this->cvTern(null, $k)], true);
        }
        foreach ([1, 9, '1', 'x', 1.5] as $k) {
            try {
                $out[] = var_export([$this->strIsset($k), $this->strOffset($k)], true);
            } catch (Throwable $e) {
                $out[] = get_class($e) . ': ' . $e->getMessage();
            }
        }
        try {
            $out[] = var_export($this->objRead('k'), true);
        } catch (Throwable $e) {
            $out[] = get_class($e) . ': ' . $e->getMessage();
        }
        $o = new ArrayObject([]);
        foreach ([[1, 1], ['1', 1], [$o, $o], [[1], [1]], [null, false]] as [$x, $y]) {
            $out[] = var_export($this->same($x, $y), true);
        }
        foreach ([0, '0', '', 'a', [], [0], 0.0, null, $o] as $v) {
            $out[] = $this->truthy($v) . var_export($this->negate($v), true) . var_export($this->typed($v), true);
        }
        return implode("\n", $out);
    }
}
$battery = new Battery();
for ($i = 0; $i < 2; $i++) {
    echo $battery->run(), "\n";
}

function lookup($a, $k) { if (isset($a[$k])) { return $a[$k]; } return 'none'; }
function lookup_tern($a, $k) { return isset($a[$k]) ? $a[$k] : 'd'; }
function lookup_neg($a, $k) { if (!isset($a[$k])) { return 'neg'; } return $a[$k]; }
function lookups($arr, $k) { return [lookup($arr, $k), lookup_tern($arr, $k), lookup_neg($arr, $k)]; }
$lookup_array = ['x' => 'X', 3 => 'three'];
foreach (['x', 1.5, null, true, 1.5, null] as $k) {
    echo json_encode(lookups($lookup_array, $k)), "\n";
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

Warning: Undefined array key "zzz" in %s on line 9
NULL

Warning: Undefined property: Registry::$undeclared in %s on line 11
NULL
int(1)

Warning: Undefined array key "zzz" in %s on line 9
NULL

Warning: Undefined property: Registry::$undeclared in %s on line 11
NULL
int(2)

Warning: Undefined variable $undefinedVariable in %s on line 74
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

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 131

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 132

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 133

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 134

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 136

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 137

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 131

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 133

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 134

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 136

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 137

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 135

Warning: Undefined array key 1 in %s on line 135

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 135

Warning: Undefined array key "" in %s on line 135

Warning: Undefined array key "missing" in %s on line 135

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 145

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 146

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 147

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 148

Warning: Undefined array key 1 in %s on line 148

Warning: Undefined array key 1 in %s on line 148

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 145

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 146

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 147

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 148

Warning: Undefined array key "" in %s on line 148

Warning: Undefined array key "missing" in %s on line 148

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 145

Warning: Uninitialized string offset 9 in %s on line 138

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 139

Warning: String offset cast occurred in %s on line 138
array (
  0 => true,
  1 => true,
  2 => false,
  3 => 'full',
  4 => 'X',
  5 => 'X',
)
array (
  0 => true,
  1 => true,
  2 => false,
  3 => 'full',
  4 => 'three',
  5 => 'three',
)
array (
  0 => true,
  1 => true,
  2 => false,
  3 => 'full',
  4 => 'three',
  5 => 'three',
)
array (
  0 => false,
  1 => false,
  2 => true,
  3 => 'empty',
  4 => 'dflt',
  5 => 'none',
)
array (
  0 => false,
  1 => false,
  2 => true,
  3 => 'empty',
  4 => 'dflt',
  5 => 'none',
)
array (
  0 => false,
  1 => false,
  2 => true,
  3 => 'empty',
  4 => 'dflt',
  5 => 'none',
)
array (
  0 => false,
  1 => false,
  2 => true,
  3 => 'empty',
  4 => 'dflt',
  5 => 'none',
)
array (
  0 => true,
  1 => true,
  2 => false,
  3 => 'empty',
  4 => 0,
  5 => 0,
)
array (
  0 => false,
  1 => false,
  2 => true,
  3 => 'empty',
  4 => 'dflt',
  5 => 'none',
)
'X'
NULL
NULL
NULL
array (
  0 => 1,
  1 => 'X',
  2 => 'f',
  3 => 'same',
)
array (
  0 => 1,
  1 => 'three',
  2 => 'f',
  3 => 'other',
)
array (
  0 => 0,
  1 => 'n',
  2 => 'e',
  3 => 'other',
)
array (
  0 => 0,
  1 => 'n',
  2 => 'e',
  3 => 'other',
)
array (
  0 => 0,
  1 => 'n',
  2 => 'e',
  3 => 'other',
)
array (
  0 => 0,
  1 => 'n',
  2 => 'e',
  3 => 'other',
)
array (
  0 => 0,
  1 => 0,
  2 => 'n',
)
array (
  0 => 1,
  1 => 0,
  2 => 'n',
)
array (
  0 => 1,
  1 => 0,
  2 => 'n',
)
array (
  0 => true,
  1 => 'e',
)
array (
  0 => false,
  1 => '',
)
array (
  0 => true,
  1 => 'e',
)
TypeError: Cannot access offset of type string on string
array (
  0 => true,
  1 => 'e',
)
Error: Cannot use object of type stdClass as array
true
false
true
true
false
notruefalse
notruetrue
notruetrue
yesfalsetrue
notruefalse
yesfalsefalse
notruefalse
notruefalse
yesfalsefalse

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 131

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 132

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 133

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 134

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 136

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 137

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 131

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 133

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 134

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 136

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 137

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 135

Warning: Undefined array key 1 in %s on line 135

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 135

Warning: Undefined array key "" in %s on line 135

Warning: Undefined array key "missing" in %s on line 135

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 145

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 146

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 147

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 148

Warning: Undefined array key 1 in %s on line 148

Warning: Undefined array key 1 in %s on line 148

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 145

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 146

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 147

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 148

Warning: Undefined array key "" in %s on line 148

Warning: Undefined array key "missing" in %s on line 148

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 145

Warning: Uninitialized string offset 9 in %s on line 138

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 139

Warning: String offset cast occurred in %s on line 138
array (
  0 => true,
  1 => true,
  2 => false,
  3 => 'full',
  4 => 'X',
  5 => 'X',
)
array (
  0 => true,
  1 => true,
  2 => false,
  3 => 'full',
  4 => 'three',
  5 => 'three',
)
array (
  0 => true,
  1 => true,
  2 => false,
  3 => 'full',
  4 => 'three',
  5 => 'three',
)
array (
  0 => false,
  1 => false,
  2 => true,
  3 => 'empty',
  4 => 'dflt',
  5 => 'none',
)
array (
  0 => false,
  1 => false,
  2 => true,
  3 => 'empty',
  4 => 'dflt',
  5 => 'none',
)
array (
  0 => false,
  1 => false,
  2 => true,
  3 => 'empty',
  4 => 'dflt',
  5 => 'none',
)
array (
  0 => false,
  1 => false,
  2 => true,
  3 => 'empty',
  4 => 'dflt',
  5 => 'none',
)
array (
  0 => true,
  1 => true,
  2 => false,
  3 => 'empty',
  4 => 0,
  5 => 0,
)
array (
  0 => false,
  1 => false,
  2 => true,
  3 => 'empty',
  4 => 'dflt',
  5 => 'none',
)
'X'
NULL
NULL
NULL
array (
  0 => 1,
  1 => 'X',
  2 => 'f',
  3 => 'same',
)
array (
  0 => 1,
  1 => 'three',
  2 => 'f',
  3 => 'other',
)
array (
  0 => 0,
  1 => 'n',
  2 => 'e',
  3 => 'other',
)
array (
  0 => 0,
  1 => 'n',
  2 => 'e',
  3 => 'other',
)
array (
  0 => 0,
  1 => 'n',
  2 => 'e',
  3 => 'other',
)
array (
  0 => 0,
  1 => 'n',
  2 => 'e',
  3 => 'other',
)
array (
  0 => 0,
  1 => 0,
  2 => 'n',
)
array (
  0 => 1,
  1 => 0,
  2 => 'n',
)
array (
  0 => 1,
  1 => 0,
  2 => 'n',
)
array (
  0 => true,
  1 => 'e',
)
array (
  0 => false,
  1 => '',
)
array (
  0 => true,
  1 => 'e',
)
TypeError: Cannot access offset of type string on string
array (
  0 => true,
  1 => 'e',
)
Error: Cannot use object of type stdClass as array
true
false
true
true
false
notruefalse
notruetrue
notruetrue
yesfalsetrue
notruefalse
yesfalsefalse
notruefalse
notruefalse
yesfalsefalse
["X","X","X"]

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 191

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 192

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 193
["none","d","neg"]

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 191

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 192

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 193
["none","d","neg"]
["none","d","neg"]

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 191

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 192

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 193
["none","d","neg"]

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 191

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 192

Deprecated: Using null as an array offset is deprecated, use an empty string instead in %s on line 193
["none","d","neg"]
int(100)
