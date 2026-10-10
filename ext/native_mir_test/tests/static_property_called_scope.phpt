--TEST--
Native: static:: property accesses cache per called scope
--DESCRIPTION--
static::$name resolves against the called scope; the cache slot keeps the
property of one class and other called scopes look theirs up. Inherited
and redeclared properties, writes, instance contexts, typed properties
before initialization and undeclared properties.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
namespace NativeStaticScope;

class A {
    public static $n = 'A';
    static function name() { return static::$n; }
    static function set($v) { static::$n = $v; }
    static function append($v) { static::$n .= $v; }
    function inst() { return static::$n; }
    static function missing() { return static::$missing; }
}
class B extends A { public static $n = 'B'; }
class C extends A {}
class T {
    public static int $t;
    static function get() { return static::$t; }
}
class U extends T {}

for ($i = 0; $i < 3; $i++) {
    echo A::name(), B::name(), C::name(), (new B)->inst(), (new C)->inst(), ' ';
}
echo "\n";
C::set('c');
B::set('b');
echo A::name(), B::name(), C::name(), "\n";
B::append('+');
A::append('-');
echo A::name(), B::name(), C::name(), A::$n, B::$n, "\n";
try {
    U::get();
} catch (\Error $e) {
    echo $e->getMessage(), "\n";
}
T::$t = 5;
echo T::get(), U::get(), "\n";
try {
    B::missing();
} catch (\Error $e) {
    echo $e->getMessage(), "\n";
}
?>
--EXPECT--
ABABA ABABA ABABA 
cbc
c-b+c-c-b+
Typed static property NativeStaticScope\T::$t must not be accessed before initialization
55
Access to undeclared static property NativeStaticScope\B::$missing
