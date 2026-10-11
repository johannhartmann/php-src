--TEST--
Native FETCH_CLASS_CONSTANT answers from the run-time cache
--DESCRIPTION--
Enum cases, computed and counted class constants of literal classes, self::, parent::, static:: and fetched classes read the cache the helper fills, as the VM does. Deprecated, private, missing, trait constants and dynamic names keep their diagnostics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
enum Suit: string {
    case Hearts = 'H';
    case Spades = 'S';
    const Wild = self::Spades;
    public static function pick() { return [self::Hearts, static::Spades, self::Wild]; }
}
enum Plain { case A; case B; }
class Base {
    const LIST = ['a', 'b'];
    const NAME = 'base';
    const COMPUTED = self::LIST + [2 => 'c'];
    public static function own() { return [self::LIST, static::NAME, self::COMPUTED]; }
}
class Child extends Base {
    const NAME = 'child';
    #[\Deprecated]
    const OLD = 'old';
    private const SECRET = 's';
    public static function parents() { return [parent::NAME, parent::LIST, static::NAME, self::SECRET]; }
}
trait Tr { const T = 1; }
function dyn($class, $name) { return constant("$class::$name"); }
function run() {
    $out = [];
    for ($i = 0; $i < 3; $i++) {
        $out[] = Suit::Hearts === Suit::from('H');
        $out[] = Plain::B;
        $out[] = Suit::pick();
        $out[] = Base::own();
        $out[] = Child::own();
        $out[] = Child::parents();
        $out[] = Child::OLD;
        $out[] = Base::COMPUTED;
        $class = $i % 2 ? 'Child' : 'Base';
        $out[] = $class::NAME;
        $out[] = $class::LIST;
        $name = 'NAME';
        $out[] = Child::{$name};
        $out[] = $class::{$name};
        $obj = new $class; $out[] = $obj::NAME;
        try { $out[] = Child::SECRET; } catch (Error $e) { $out[] = $e->getMessage(); }
        try { $out[] = Base::MISSING; } catch (Error $e) { $out[] = $e->getMessage(); }
        try { $out[] = Missing::X; } catch (Error $e) { $out[] = $e->getMessage(); }
        try { $out[] = Tr::T; } catch (Error $e) { $out[] = $e->getMessage(); }
    }
    return $out;
}
set_error_handler(function ($no, $msg) { echo "E: $msg\n"; return true; });
foreach (run() as $v) {
    echo is_object($v) ? get_class($v) . '::' . $v->name : json_encode($v), "\n";
}
--EXPECT--
E: Constant Child::OLD is deprecated
E: Constant Child::OLD is deprecated
E: Constant Child::OLD is deprecated
true
Plain::B
["H","S","S"]
[["a","b"],"base",["a","b","c"]]
[["a","b"],"child",["a","b","c"]]
["base",["a","b"],"child","s"]
"old"
["a","b","c"]
"base"
["a","b"]
"child"
"base"
"base"
"Cannot access private constant Child::SECRET"
"Undefined constant Base::MISSING"
"Class \"Missing\" not found"
"Cannot access trait constant Tr::T directly"
true
Plain::B
["H","S","S"]
[["a","b"],"base",["a","b","c"]]
[["a","b"],"child",["a","b","c"]]
["base",["a","b"],"child","s"]
"old"
["a","b","c"]
"child"
["a","b"]
"child"
"child"
"child"
"Cannot access private constant Child::SECRET"
"Undefined constant Base::MISSING"
"Class \"Missing\" not found"
"Cannot access trait constant Tr::T directly"
true
Plain::B
["H","S","S"]
[["a","b"],"base",["a","b","c"]]
[["a","b"],"child",["a","b","c"]]
["base",["a","b"],"child","s"]
"old"
["a","b","c"]
"base"
["a","b"]
"child"
"base"
"base"
"Cannot access private constant Child::SECRET"
"Undefined constant Base::MISSING"
"Class \"Missing\" not found"
"Cannot access trait constant Tr::T directly"
