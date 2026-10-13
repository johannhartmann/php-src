--TEST--
self:: and parent:: in closures and trait methods resolve in the caller's scope (OPcache)
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class PA { public static function m() { return 'A'; } public function i() { return 'iA'; } }
class PB { public static function m() { return 'B'; } public function i() { return 'iB'; } }
class A extends PA {
    public static function mk() { return function () { return parent::m() . self::class . static::class; }; }
    public function mki() { return function () { return parent::i(); }; }
}
class B extends PB {}
trait T {
    public function run() { return parent::m() . self::n() . parent::i(); }
    public static function s() { return parent::m() . self::class; }
    public function k() { return function () { return parent::m(); }; }
}
class X extends PA { use T; public static function n() { return 'x'; } }
class Y extends PB { use T; public static function n() { return 'y'; } }

$top = function () { return parent::m() . self::class; };
$ta = Closure::bind($top, null, A::class);
$tb = Closure::bind($top, null, B::class);
$f = A::mk();
$fb = Closure::bind($f, null, B::class);
$g = (new A)->mki();
$gb = Closure::bind($g, new B, B::class);
$x = new X;
$y = new Y;
for ($i = 0; $i < 4; $i++) {
    echo $ta(), ' ', $tb(), ' ', $f(), ' ', $fb(), ' ', $g(), ' ', $gb(), ' ',
        $x->run(), ' ', $y->run(), ' ', X::s(), ' ', Y::s(), ' ',
        ($x->k())(), ' ', ($y->k())(), "\n";
}
?>
--EXPECT--
AA BB AAA BBB iA iB AxiA ByiB AX BY A B
AA BB AAA BBB iA iB AxiA ByiB AX BY A B
AA BB AAA BBB iA iB AxiA ByiB AX BY A B
AA BB AAA BBB iA iB AxiA ByiB AX BY A B
