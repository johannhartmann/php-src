--TEST--
Native fast new of immutable classes whose constants are evaluated
--DESCRIPTION--
An immutable class keeps ZEND_ACC_CONSTANTS_UPDATED in its per-request
mutable data once its constant-expression defaults are evaluated; the
fast new accepts it there as well as in the class flags. Objects built
on the fast path carry the evaluated defaults, with and without a
constructor.
--EXTENSIONS--
opcache
native_mir_test
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
const BASE = 40;
class Plain { const K = BASE + 2; public $v = self::K; public $a = [self::K, BASE]; }
class Ctor { const K = BASE * 2; public $v = self::K; public $w; function __construct($w) { $this->w = $w + self::K; } }
function make($i) {
    $p = new Plain;
    $c = new Ctor($i);
    return $p->v + $p->a[1] + $c->v + $c->w;
}
$sum = 0;
for ($i = 0; $i < 20; $i++) {
    $sum += make($i);
}
var_dump($sum, new Plain == new Plain, (new Ctor(1))->w);
?>
--EXPECT--
int(5030)
bool(true)
int(81)
