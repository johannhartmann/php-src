--TEST--
Native fast static-method and new sites re-armed after the epoch advances
--DESCRIPTION--
Stale fast sites of static methods (named class, self::, parent::,
static::, and a parent:: call keeping $this) and of new C, with and
without a constructor, are re-armed when the class name still binds the
immutable class they were published for. Values live in registers
across the stale sites survive, and results match the VM.
--EXTENSIONS--
opcache
native_mir_test
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Base {
    public $v = 3;
    static function twice($x) { return 2 * $x; }
    function scaled($x) { return $this->v * $x; }
    static function make() { return new static; }
    static function name() { return static::class; }
}
class Child extends Base {
    function viaSelf($x) { return self::twice($x) + parent::scaled($x); }
    function viaStatic() { return static::name(); }
}
class NoCtor { public $a = [1, 2]; }
class WithCtor { public $s; function __construct($s) { $this->s = "<$s>"; } }
function invalidate() { if (function_exists('native_mir_test_call_cache_invalidate')) { native_mir_test_call_cache_invalidate(); } }
function run($i) {
    $f = 0.5 * $i; $g = $f + 1.25; $c = new Child;
    $out = [];
    for ($k = 0; $k < 3; $k++) {
        $n = new NoCtor; $w = new WithCtor($k);
        $out[] = Base::twice($k) . '|' . $c->viaSelf($k) . '|' . $c->viaStatic()
            . '|' . get_class(Child::make()) . '|' . count($n->a) . '|' . $w->s;
        invalidate();
        $out[] = sprintf('%.2f %.2f', $f + $k, $g * $k);
    }
    return implode(' ', $out);
}
for ($i = 0; $i < 3; $i++) { echo run($i), "\n"; }
?>
--EXPECT--
0|0|Child|Child|2|<0> 0.00 0.00 2|5|Child|Child|2|<1> 1.00 1.25 4|10|Child|Child|2|<2> 2.00 2.50
0|0|Child|Child|2|<0> 0.50 0.00 2|5|Child|Child|2|<1> 1.50 1.75 4|10|Child|Child|2|<2> 2.50 3.50
0|0|Child|Child|2|<0> 1.00 0.00 2|5|Child|Child|2|<1> 2.00 2.25 4|10|Child|Child|2|<2> 3.00 4.50
