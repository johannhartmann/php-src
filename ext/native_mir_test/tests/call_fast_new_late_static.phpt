--TEST--
Native fast new sites of self, parent and static
--DESCRIPTION--
new self, new parent and new static take the fast path for the class the
site was published for and the universal protocol for any other class the
caller names (a subclass, a closure bound to another scope), also after
the epoch advances. Constructors that throw, classes without a
constructor and an internal parent constructor behave as in the VM.
--EXTENSIONS--
opcache
native_mir_test
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Coll {
    public $items;
    function __construct($items) { $this->items = $items; }
    function map($f) { return new static(array_map($f, $this->items)); }
    static function make($items) { return new self($items); }
    static function late($items) { return new static($items); }
}
class Sub extends Coll {
    function up($x) { return new parent([$x]); }
}
class Strict extends Coll {
    function __construct($items) {
        if (count($items) > 2) { throw new LengthException('too many'); }
        parent::__construct($items);
    }
}
class NoCtor { public $n = 5; static function mk() { return new static; } }
class NoCtor2 extends NoCtor { public $n = 6; }
class Bag extends ArrayObject { static function mk($a) { return new static($a); } }
function invalidate() { if (function_exists('native_mir_test_call_cache_invalidate')) { native_mir_test_call_cache_invalidate(); } }
function show($o) { return get_class($o) . '(' . implode(',', $o->items) . ')'; }
function run($i) {
    $f = 0.5 * $i; $g = $f + 1.25;
    $out = [];
    $twice = fn($v) => 2 * $v;
    $maker = Closure::bind(fn($v) => new static([$v, $v]), null, Sub::class);
    for ($k = 0; $k < 4; $k++) {
        $c = $k % 2 ? new Sub([$k, $i]) : new Coll([$k, $i]);
        $out[] = show($c->map($twice)) . ' ' . show(Coll::make([$k])) . ' ' . show(Sub::make([$k]))
            . ' ' . show(Coll::late([$k])) . ' ' . show(Sub::late([$k])) . ' ' . show((new Sub([]))->up($k))
            . ' ' . show($maker($k)) . ' ' . get_class(NoCtor::mk()) . NoCtor::mk()->n
            . ' ' . get_class(NoCtor2::mk()) . NoCtor2::mk()->n . ' ' . get_class(Bag::mk([$k])) . count(Bag::mk([$k, $i]));
        try {
            $s = new Strict(array_fill(0, $k, $i));
            $out[] = show($s->map($twice));
            $out[] = show(Strict::late(array_fill(0, $k + 1, $k)));
        } catch (LengthException $e) {
            $out[] = 'E:' . $e->getMessage() . '@' . $e->getLine();
        }
        if ($k == 1) { invalidate(); }
        $out[] = sprintf('%.2f %.2f', $f + $k, $g * $k);
    }
    return implode(' | ', $out);
}
for ($i = 0; $i < 3; $i++) { echo run($i), "\n"; }
?>
--EXPECT--
Coll(0,0) Coll(0) Coll(0) Coll(0) Sub(0) Coll(0) Sub(0,0) NoCtor5 NoCtor26 Bag2 | Strict() | Strict(0) | 0.00 0.00 | Sub(2,0) Coll(1) Coll(1) Coll(1) Sub(1) Coll(1) Sub(1,1) NoCtor5 NoCtor26 Bag2 | Strict(0) | Strict(1,1) | 1.00 1.25 | Coll(4,0) Coll(2) Coll(2) Coll(2) Sub(2) Coll(2) Sub(2,2) NoCtor5 NoCtor26 Bag2 | Strict(0,0) | E:too many@14 | 2.00 2.50 | Sub(6,0) Coll(3) Coll(3) Coll(3) Sub(3) Coll(3) Sub(3,3) NoCtor5 NoCtor26 Bag2 | E:too many@14 | 3.00 3.75
Coll(0,2) Coll(0) Coll(0) Coll(0) Sub(0) Coll(0) Sub(0,0) NoCtor5 NoCtor26 Bag2 | Strict() | Strict(0) | 0.50 0.00 | Sub(2,2) Coll(1) Coll(1) Coll(1) Sub(1) Coll(1) Sub(1,1) NoCtor5 NoCtor26 Bag2 | Strict(2) | Strict(1,1) | 1.50 1.75 | Coll(4,2) Coll(2) Coll(2) Coll(2) Sub(2) Coll(2) Sub(2,2) NoCtor5 NoCtor26 Bag2 | Strict(2,2) | E:too many@14 | 2.50 3.50 | Sub(6,2) Coll(3) Coll(3) Coll(3) Sub(3) Coll(3) Sub(3,3) NoCtor5 NoCtor26 Bag2 | E:too many@14 | 3.50 5.25
Coll(0,4) Coll(0) Coll(0) Coll(0) Sub(0) Coll(0) Sub(0,0) NoCtor5 NoCtor26 Bag2 | Strict() | Strict(0) | 1.00 0.00 | Sub(2,4) Coll(1) Coll(1) Coll(1) Sub(1) Coll(1) Sub(1,1) NoCtor5 NoCtor26 Bag2 | Strict(4) | Strict(1,1) | 2.00 2.25 | Coll(4,4) Coll(2) Coll(2) Coll(2) Sub(2) Coll(2) Sub(2,2) NoCtor5 NoCtor26 Bag2 | Strict(4,4) | E:too many@14 | 3.00 4.50 | Sub(6,4) Coll(3) Coll(3) Coll(3) Sub(3) Coll(3) Sub(3,3) NoCtor5 NoCtor26 Bag2 | E:too many@14 | 4.00 6.75
