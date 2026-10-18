--TEST--
Native: static trait method calls and "Class::method" callables deprecate on every call
--DESCRIPTION--
As the VM never caches them, these lookups keep the universal protocol on
every call so that each emits its deprecation: no resolution cache entry,
fast site or recorded dynamic target holds them.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
trait T {
    public static function foo() { return "T::foo"; }
}
class C { use T; }
class A { public static function foo() { return "A::foo"; } }
class B extends A {
    public static function foo() { return "B::foo"; }
    function call($c) { return call_user_func($c); }
    function dyn($c) { return $c(); }
}
function trait_static() { return T::foo(); }
function trait_class($class) { return $class::foo(); }
for ($i = 0; $i < 3; $i++) {
    echo trait_static(), "\n";
    echo trait_class('T'), "\n";
    echo trait_class('C'), "\n";
    $b = new B;
    echo $b->call([$b, 'parent::foo']), "\n";

    echo $b->call(["B", "A::foo"]), "\n";
}
?>
--EXPECTF--
T::foo

Deprecated: Calling static trait method T::foo is deprecated, it should only be called on a class using the trait in %s on line 13
T::foo
T::foo

Deprecated: Callables of the form ["B", "parent::foo"] are deprecated in %s on line 9
A::foo

Deprecated: Callables of the form ["B", "A::foo"] are deprecated in %s on line 9
A::foo
T::foo

Deprecated: Calling static trait method T::foo is deprecated, it should only be called on a class using the trait in %s on line 13
T::foo
T::foo

Deprecated: Callables of the form ["B", "parent::foo"] are deprecated in %s on line 9
A::foo

Deprecated: Callables of the form ["B", "A::foo"] are deprecated in %s on line 9
A::foo
T::foo

Deprecated: Calling static trait method T::foo is deprecated, it should only be called on a class using the trait in %s on line 13
T::foo
T::foo

Deprecated: Callables of the form ["B", "parent::foo"] are deprecated in %s on line 9
A::foo

Deprecated: Callables of the form ["B", "A::foo"] are deprecated in %s on line 9
A::foo
