--TEST--
Native compound assignments to properties, static properties and elements run in place
--DESCRIPTION--
op= on an object property, a static property and an array element computes in place as the VM does, so .= extends an owned string. Typed properties, typed references, readonly properties, magic accessors, ArrayAccess, references and exceptions keep stock semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class T { public function __toString(): string { return "T"; } }
class Thrower { public function __toString(): string { throw new Exception("ts"); } }
class C {
    public $buf = '';
    public int $n = 1;
    public string $s = 'a';
    public int|string $u = 'x';
    public ?array $arr = [];
    public readonly int $ro;
    public static $sbuf = '';
    public static int $sn = 3;
    public static string $ss = 's';
    public function __construct() { $this->ro = 1; }
    public function build($pieces) {
        foreach ($pieces as $p) { $this->buf .= $p; }
        return $this->buf;
    }
}
class M {
    private $data = ['p' => 'm'];
    public function __get($n) { echo "get $n|"; return $this->data[$n] ?? null; }
    public function __set($n, $v) { echo "set $n|"; $this->data[$n] = $v; }
}
class AA implements ArrayAccess {
    public $d = ['k' => 'v'];
    public function offsetExists($o): bool { return isset($this->d[$o]); }
    public function offsetGet($o): mixed { echo "oget|"; return $this->d[$o] ?? null; }
    public function offsetSet($o, $v): void { echo "oset|"; $this->d[$o] = $v; }
    public function offsetUnset($o): void {}
}
function show($label, $v) { echo $label, ": ", json_encode($v), "\n"; }
function run($r) {
    $c = new C;
    show('build', $c->build(['a', 'b', 1, 2.5, true, null, new T]));
    $c->n += 2; show('n', $c->n);
    $c->n .= '5'; show('n.=', $c->n);
    try { $c->n .= 'x'; } catch (TypeError $e) { show('n.=x', $e->getMessage()); }
    show('n after', $c->n);
    $c->s .= 'b'; $c->s .= 1; show('s', $c->s);
    $c->u .= 'y'; show('u', $c->u);
    $c->u = 5; $c->u .= 1; show('u int', $c->u);
    $c->arr += ['a' => 1]; show('arr', $c->arr);
    try { $c->ro .= 'x'; } catch (Error $e) { show('ro', $e->getMessage()); }
    try { $c->ro += 1; } catch (Error $e) { show('ro+', $e->getMessage()); }
    $ref = &$c->s; $ref .= 'r'; show('ref', $c->s);
    $ref2 = &$c->n;
    try { $ref2 .= 'z'; } catch (TypeError $e) { show('ref2', $e->getMessage()); }
    show('ref2 after', $c->n);
    $res = ($c->buf .= '!'); show('res', $res);
    $res = ($c->s .= '?'); show('res2', $res);
    try { $c->buf .= new Thrower; } catch (Exception $e) { show('thrower', $e->getMessage()); }
    show('buf after thrower', $c->buf);
    $m = new M; $m->p .= 'x'; $m->q .= 'y'; show('magic', [$m->p, $m->q]);
    $name = 'buf'; $c->$name .= '#'; show('dyn', $c->buf);
    $o = new stdClass; $o->dyn = 'd'; $o->dyn .= 'e'; $o->undef .= 'u'; show('std', (array) $o);
    C::$sbuf = ''; for ($i = 0; $i < 3; $i++) { C::$sbuf .= $i; } show('sbuf', C::$sbuf);
    C::$sn += 4; show('sn', C::$sn);
    try { C::$sn .= 'q'; } catch (TypeError $e) { show('sn.=q', $e->getMessage()); }
    show('sn after', C::$sn);
    C::$ss .= 'x'; show('ss', C::$ss);
    $sr = &C::$ss; $sr .= 'y'; show('ss ref', C::$ss);
    $res = (C::$sbuf .= 'end'); show('sres', $res);
    $a = ['k' => 'a', 'n' => 1];
    for ($i = 0; $i < 3; $i++) { $a['k'] .= $i; $a['n'] += $i; $a['new' . $i] .= 'z'; }
    show('arr', $a);
    $a['k'] .= $a['k']; show('self', $a['k']);
    $b = $a; $b['k'] .= '!'; show('cow', [$a['k'], $b['k']]);
    $x = &$a['k']; $x .= 'R'; $a['k'] .= 'S'; show('elref', [$x, $a['k']]);
    $nested = ['a' => ['b' => 's']]; $nested['a']['b'] .= 't'; $nested['a']['c'] .= 'u'; show('nested', $nested);
    $aa = new AA; $aa['k'] .= 'w'; try { $aa["k"] += 1; } catch (TypeError $e) { show("aa+", $e->getMessage()); } show('aa', $aa->d);
    try { $a['n'] /= 0; } catch (DivisionByZeroError $e) { show('div', $e->getMessage()); }
    show('n', $a['n']);
    $c->arr = null;
    $c->arr[] = 1; show('null arr', $c->arr);
    $l = [1]; $l[] .= 'p'; show('append', $l);
    $cv = 's'; $cv .= 1; $cv .= 2.0; $cv .= null; show('cv', $cv);
    $u = 0; $u **= 2; $u <<= 1; $u |= 6; $u ^= 1; $u &= 3; $u %= 2; $u >>= 1; $u -= 1; $u *= 3; show('ops', $u);
}
run(1);
run(2);
--EXPECTF--
build: "ab12.51T"
n: 3
n.=: 35
n.=x: "Cannot assign string to property C::$n of type int"
n after: 35
s: "ab1"
u: "xy"
u int: "51"
arr: {"a":1}
ro: "Cannot modify readonly property C::$ro"
ro+: "Cannot modify readonly property C::$ro"
ref: "ab1r"
ref2: "Cannot assign string to reference held by property C::$n of type int"
ref2 after: 35
res: "ab12.51T!"
res2: "ab1r?"
thrower: "ts"
buf after thrower: "ab12.51T!"
get p|set p|get q|set q|get p|get q|magic: ["mx","y"]
dyn: "ab12.51T!#"

Warning: Undefined property: stdClass::$undef in %s on line 56
std: {"dyn":"de","undef":"u"}
sbuf: "012"
sn: 7
sn.=q: "Cannot assign string to property C::$sn of type int"
sn after: 7
ss: "sx"
ss ref: "sxy"
sres: "012end"

Warning: Undefined array key "new0" in %s on line 65

Warning: Undefined array key "new1" in %s on line 65

Warning: Undefined array key "new2" in %s on line 65
arr: {"k":"a012","n":4,"new0":"z","new1":"z","new2":"z"}
self: "a012a012"
cow: ["a012a012","a012a012!"]
elref: ["a012a012RS","a012a012RS"]

Warning: Undefined array key "c" in %s on line 70
nested: {"a":{"b":"st","c":"u"}}
oget|oset|oget|aa+: "Unsupported operand types: string + int"
aa: {"k":"vw"}
div: "Division by zero"
n: 4
null arr: [1]
append: [1,"p"]
cv: "s12"
ops: -3
build: "ab12.51T"
n: 3
n.=: 35
n.=x: "Cannot assign string to property C::$n of type int"
n after: 35
s: "ab1"
u: "xy"
u int: "51"
arr: {"a":1}
ro: "Cannot modify readonly property C::$ro"
ro+: "Cannot modify readonly property C::$ro"
ref: "ab1r"
ref2: "Cannot assign string to reference held by property C::$n of type int"
ref2 after: 35
res: "ab12.51T!"
res2: "ab1r?"
thrower: "ts"
buf after thrower: "ab12.51T!"
get p|set p|get q|set q|get p|get q|magic: ["mx","y"]
dyn: "ab12.51T!#"

Warning: Undefined property: stdClass::$undef in %s on line 56
std: {"dyn":"de","undef":"u"}
sbuf: "012"
sn: 11
sn.=q: "Cannot assign string to property C::$sn of type int"
sn after: 11
ss: "sxyx"
ss ref: "sxyxy"
sres: "012end"

Warning: Undefined array key "new0" in %s on line 65

Warning: Undefined array key "new1" in %s on line 65

Warning: Undefined array key "new2" in %s on line 65
arr: {"k":"a012","n":4,"new0":"z","new1":"z","new2":"z"}
self: "a012a012"
cow: ["a012a012","a012a012!"]
elref: ["a012a012RS","a012a012RS"]

Warning: Undefined array key "c" in %s on line 70
nested: {"a":{"b":"st","c":"u"}}
oget|oset|oget|aa+: "Unsupported operand types: string + int"
aa: {"k":"vw"}
div: "Division by zero"
n: 4
null arr: [1]
append: [1,"p"]
cv: "s12"
ops: -3
