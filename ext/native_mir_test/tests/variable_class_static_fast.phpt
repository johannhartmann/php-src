--TEST--
Native: $class::m(...) takes the static method recorded for each fetched class
--DESCRIPTION--
A literal static method of a class fetched from a variable, as Symfony's
container calls each service factory, finds the target the resolver
recorded for the class, the method name and the calling scope: late static
binding, overrides, by-reference parameters, visibility from the calling
scope, private methods, __callStatic, instance methods called statically
with and without a compatible $this, and unknown classes.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
namespace NativeVariableClass;

class Base {
    static function make($c, $x) { return static::class . ':' . get_class($c) . ':' . $x; }
    static function bump(&$v) { $v++; return static::class; }
    protected static function hidden() { return 'hidden:' . static::class; }
    private static function secret() { return 'secret'; }
    function inst() { return 'inst:' . static::class . ':' . (isset($this) ? get_class($this) : '-'); }
    static function callSecret($class) { return $class::secret(); }
}
class F1 extends Base {}
class F2 extends Base { static function make($c, $x) { return 'F2!' . parent::make($c, $x); } }
class F3 extends Base {}
class M { static function __callStatic($n, $a) { return "magic $n(" . count($a) . ")"; } }
class Caller extends Base {
    function load($class, $x) { return $class::make($this, $x); }
    function inner($class) { return $class::inst(); }
    function hide($class) { return $class::hidden(); }
}
function load($class, $x) { $o = new \stdClass; return $class::make($o, $x); }
function bump($class) { $v = 1; $r = $class::bump($v); return "$r/$v"; }
function hide($class) { return $class::hidden(); }
function magic($class) { return $class::make(1, 2); }
function factory($name) { $class = __NAMESPACE__ . '\\' . substr($name . 'xxxx', 0, -4); return $class::make(new Caller, strlen($name)); }

$c = new Caller;
for ($round = 0; $round < 3; $round++) {
    $out = [];
    foreach ([F1::class, F2::class, F3::class, F1::class, Caller::class] as $cls) {
        $out[] = load($cls, $round);
        $out[] = $c->load($cls, $round);
        $out[] = bump($cls);
        $out[] = $c->hide($cls);
        $out[] = factory(substr(strrchr($cls, '\\'), 1));
    }
    $out[] = $c->inner(Caller::class);
    $out[] = $c->inner(Base::class);
    $out[] = magic(M::class);
    $out[] = Base::callSecret(F1::class);
    try { hide(F1::class); } catch (\Error $e) { $out[] = get_class($e) . ': ' . $e->getMessage(); }
    try { $c->inner(F1::class); } catch (\Error $e) { $out[] = get_class($e) . ': ' . $e->getMessage(); }
    try { load(M::class . 'X', 1); } catch (\Error $e) { $out[] = get_class($e) . ': ' . $e->getMessage(); }
    echo implode("\n", $out), "\n--\n";
}
?>
--EXPECT--
NativeVariableClass\F1:stdClass:0
NativeVariableClass\F1:NativeVariableClass\Caller:0
NativeVariableClass\F1/2
hidden:NativeVariableClass\F1
NativeVariableClass\F1:NativeVariableClass\Caller:2
F2!NativeVariableClass\F2:stdClass:0
F2!NativeVariableClass\F2:NativeVariableClass\Caller:0
NativeVariableClass\F2/2
hidden:NativeVariableClass\F2
F2!NativeVariableClass\F2:NativeVariableClass\Caller:2
NativeVariableClass\F3:stdClass:0
NativeVariableClass\F3:NativeVariableClass\Caller:0
NativeVariableClass\F3/2
hidden:NativeVariableClass\F3
NativeVariableClass\F3:NativeVariableClass\Caller:2
NativeVariableClass\F1:stdClass:0
NativeVariableClass\F1:NativeVariableClass\Caller:0
NativeVariableClass\F1/2
hidden:NativeVariableClass\F1
NativeVariableClass\F1:NativeVariableClass\Caller:2
NativeVariableClass\Caller:stdClass:0
NativeVariableClass\Caller:NativeVariableClass\Caller:0
NativeVariableClass\Caller/2
hidden:NativeVariableClass\Caller
NativeVariableClass\Caller:NativeVariableClass\Caller:6
inst:NativeVariableClass\Caller:NativeVariableClass\Caller
inst:NativeVariableClass\Caller:NativeVariableClass\Caller
magic make(2)
secret
Error: Call to protected method NativeVariableClass\Base::hidden() from global scope
Error: Non-static method NativeVariableClass\Base::inst() cannot be called statically
Error: Class "NativeVariableClass\MX" not found
--
NativeVariableClass\F1:stdClass:1
NativeVariableClass\F1:NativeVariableClass\Caller:1
NativeVariableClass\F1/2
hidden:NativeVariableClass\F1
NativeVariableClass\F1:NativeVariableClass\Caller:2
F2!NativeVariableClass\F2:stdClass:1
F2!NativeVariableClass\F2:NativeVariableClass\Caller:1
NativeVariableClass\F2/2
hidden:NativeVariableClass\F2
F2!NativeVariableClass\F2:NativeVariableClass\Caller:2
NativeVariableClass\F3:stdClass:1
NativeVariableClass\F3:NativeVariableClass\Caller:1
NativeVariableClass\F3/2
hidden:NativeVariableClass\F3
NativeVariableClass\F3:NativeVariableClass\Caller:2
NativeVariableClass\F1:stdClass:1
NativeVariableClass\F1:NativeVariableClass\Caller:1
NativeVariableClass\F1/2
hidden:NativeVariableClass\F1
NativeVariableClass\F1:NativeVariableClass\Caller:2
NativeVariableClass\Caller:stdClass:1
NativeVariableClass\Caller:NativeVariableClass\Caller:1
NativeVariableClass\Caller/2
hidden:NativeVariableClass\Caller
NativeVariableClass\Caller:NativeVariableClass\Caller:6
inst:NativeVariableClass\Caller:NativeVariableClass\Caller
inst:NativeVariableClass\Caller:NativeVariableClass\Caller
magic make(2)
secret
Error: Call to protected method NativeVariableClass\Base::hidden() from global scope
Error: Non-static method NativeVariableClass\Base::inst() cannot be called statically
Error: Class "NativeVariableClass\MX" not found
--
NativeVariableClass\F1:stdClass:2
NativeVariableClass\F1:NativeVariableClass\Caller:2
NativeVariableClass\F1/2
hidden:NativeVariableClass\F1
NativeVariableClass\F1:NativeVariableClass\Caller:2
F2!NativeVariableClass\F2:stdClass:2
F2!NativeVariableClass\F2:NativeVariableClass\Caller:2
NativeVariableClass\F2/2
hidden:NativeVariableClass\F2
F2!NativeVariableClass\F2:NativeVariableClass\Caller:2
NativeVariableClass\F3:stdClass:2
NativeVariableClass\F3:NativeVariableClass\Caller:2
NativeVariableClass\F3/2
hidden:NativeVariableClass\F3
NativeVariableClass\F3:NativeVariableClass\Caller:2
NativeVariableClass\F1:stdClass:2
NativeVariableClass\F1:NativeVariableClass\Caller:2
NativeVariableClass\F1/2
hidden:NativeVariableClass\F1
NativeVariableClass\F1:NativeVariableClass\Caller:2
NativeVariableClass\Caller:stdClass:2
NativeVariableClass\Caller:NativeVariableClass\Caller:2
NativeVariableClass\Caller/2
hidden:NativeVariableClass\Caller
NativeVariableClass\Caller:NativeVariableClass\Caller:6
inst:NativeVariableClass\Caller:NativeVariableClass\Caller
inst:NativeVariableClass\Caller:NativeVariableClass\Caller
magic make(2)
secret
Error: Call to protected method NativeVariableClass\Base::hidden() from global scope
Error: Non-static method NativeVariableClass\Base::inst() cannot be called statically
Error: Class "NativeVariableClass\MX" not found
--
