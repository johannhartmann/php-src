--TEST--
Native: self:: and parent:: of trait methods and closures compare the caller's scope
--DESCRIPTION--
A trait method's code runs in every using class's scope and a closure's in
every scope it is bound to: their self::/parent:: sites take the fast path
for the scope they were published for and compare it on every call. Using
classes with different parents, subclasses, constructors, internal parent
methods, fake closures, nested closures and rebound closures, each
monomorphic and alternating.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
namespace NativeScopeForward;

class P1 { public $tag; function __construct($t) { $this->tag = "p1:$t"; } function v($x) { return "P1::v($x)/" . static::class; } static function s($x) { return "P1::s($x)/" . static::class; } }
class P2 { public $tag; function __construct($t) { $this->tag = "p2:$t"; } function v($x) { return "P2::v($x)/" . static::class; } static function s($x) { return "P2::s($x)/" . static::class; } }
trait T {
    function __construct($t) { parent::__construct("t$t"); }
    function v($x) { return 'T:' . parent::v($x) . '|' . self::k() . '|' . parent::s($x); }
    static function k() { return self::class . '/' . static::class; }
    static function sv($x) { return parent::s($x) . '#' . self::k(); }
    function inner() { return function ($x) { return parent::v($x) . '~' . self::k(); }; }
}
class A extends P1 { use T; }
class B extends P2 { use T; }
class C extends A {}
class R extends \ReflectionProperty {
    use RT;
}
trait RT { function getValue($o = null): mixed { return 'R:' . parent::getValue($o); } }
class Holder { public $p = 'held'; }

class Q1 { static function m() { return 'Q1::m/' . static::class; } function i() { return 'Q1::i'; } }
class Q2 { static function m() { return 'Q2::m/' . static::class; } function i() { return 'Q2::i'; } }
class S1 extends Q1 {} class S2 extends Q2 {}

$h = new Holder;
$r = new R(Holder::class, 'p');
$closure = function ($x) { return self::m() . ':' . parent::m() . ':' . $x; };
$static = static function ($x) { return self::m() . '/' . $x; };
function run(array $objs, $r, $h, $closure, $static) {
for ($round = 0; $round < 3; $round++) {
    $out = [];
    foreach ($objs as $o) {
        $out[] = $o->tag . ' ' . $o->v($round);
        $out[] = $o::sv($round);
        $out[] = $o->inner()($round);
        $f = $o->v(...);
        $out[] = $f("f$round");
    }
    $out[] = A::k() . ' ' . B::k() . ' ' . C::k() . ' ' . C::sv($round);
    $out[] = $r->getValue($h);
    foreach ([S1::class, S2::class, S1::class] as $cls) {
        $b = \Closure::bind($closure, null, $cls);
        $out[] = $b($round);
        $s = \Closure::bind($static, null, $cls);
        $out[] = $s($round);
        $i = \Closure::bind(function () { return self::i() . '+' . parent::i(); }, new $cls, $cls);
        $out[] = $i();
    }
    echo implode("\n", $out), "\n--\n";
}
}
run([new A(1), new A(2), new A(3)], $r, $h, $closure, $static);
run([new A(1), new B(2), new C(3), new A(4), new B(5)], $r, $h, $closure, $static);
?>
--EXPECT--
p1:t1 T:P1::v(0)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(0)/NativeScopeForward\A
P1::s(0)/NativeScopeForward\A#NativeScopeForward\A/NativeScopeForward\A
P1::v(0)/NativeScopeForward\A~NativeScopeForward\A/NativeScopeForward\A
T:P1::v(f0)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(f0)/NativeScopeForward\A
p1:t2 T:P1::v(0)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(0)/NativeScopeForward\A
P1::s(0)/NativeScopeForward\A#NativeScopeForward\A/NativeScopeForward\A
P1::v(0)/NativeScopeForward\A~NativeScopeForward\A/NativeScopeForward\A
T:P1::v(f0)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(f0)/NativeScopeForward\A
p1:t3 T:P1::v(0)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(0)/NativeScopeForward\A
P1::s(0)/NativeScopeForward\A#NativeScopeForward\A/NativeScopeForward\A
P1::v(0)/NativeScopeForward\A~NativeScopeForward\A/NativeScopeForward\A
T:P1::v(f0)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(f0)/NativeScopeForward\A
NativeScopeForward\A/NativeScopeForward\A NativeScopeForward\B/NativeScopeForward\B NativeScopeForward\A/NativeScopeForward\C P1::s(0)/NativeScopeForward\C#NativeScopeForward\A/NativeScopeForward\C
R:held
Q1::m/NativeScopeForward\S1:Q1::m/NativeScopeForward\S1:0
Q1::m/NativeScopeForward\S1/0
Q1::i+Q1::i
Q2::m/NativeScopeForward\S2:Q2::m/NativeScopeForward\S2:0
Q2::m/NativeScopeForward\S2/0
Q2::i+Q2::i
Q1::m/NativeScopeForward\S1:Q1::m/NativeScopeForward\S1:0
Q1::m/NativeScopeForward\S1/0
Q1::i+Q1::i
--
p1:t1 T:P1::v(1)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(1)/NativeScopeForward\A
P1::s(1)/NativeScopeForward\A#NativeScopeForward\A/NativeScopeForward\A
P1::v(1)/NativeScopeForward\A~NativeScopeForward\A/NativeScopeForward\A
T:P1::v(f1)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(f1)/NativeScopeForward\A
p1:t2 T:P1::v(1)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(1)/NativeScopeForward\A
P1::s(1)/NativeScopeForward\A#NativeScopeForward\A/NativeScopeForward\A
P1::v(1)/NativeScopeForward\A~NativeScopeForward\A/NativeScopeForward\A
T:P1::v(f1)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(f1)/NativeScopeForward\A
p1:t3 T:P1::v(1)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(1)/NativeScopeForward\A
P1::s(1)/NativeScopeForward\A#NativeScopeForward\A/NativeScopeForward\A
P1::v(1)/NativeScopeForward\A~NativeScopeForward\A/NativeScopeForward\A
T:P1::v(f1)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(f1)/NativeScopeForward\A
NativeScopeForward\A/NativeScopeForward\A NativeScopeForward\B/NativeScopeForward\B NativeScopeForward\A/NativeScopeForward\C P1::s(1)/NativeScopeForward\C#NativeScopeForward\A/NativeScopeForward\C
R:held
Q1::m/NativeScopeForward\S1:Q1::m/NativeScopeForward\S1:1
Q1::m/NativeScopeForward\S1/1
Q1::i+Q1::i
Q2::m/NativeScopeForward\S2:Q2::m/NativeScopeForward\S2:1
Q2::m/NativeScopeForward\S2/1
Q2::i+Q2::i
Q1::m/NativeScopeForward\S1:Q1::m/NativeScopeForward\S1:1
Q1::m/NativeScopeForward\S1/1
Q1::i+Q1::i
--
p1:t1 T:P1::v(2)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(2)/NativeScopeForward\A
P1::s(2)/NativeScopeForward\A#NativeScopeForward\A/NativeScopeForward\A
P1::v(2)/NativeScopeForward\A~NativeScopeForward\A/NativeScopeForward\A
T:P1::v(f2)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(f2)/NativeScopeForward\A
p1:t2 T:P1::v(2)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(2)/NativeScopeForward\A
P1::s(2)/NativeScopeForward\A#NativeScopeForward\A/NativeScopeForward\A
P1::v(2)/NativeScopeForward\A~NativeScopeForward\A/NativeScopeForward\A
T:P1::v(f2)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(f2)/NativeScopeForward\A
p1:t3 T:P1::v(2)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(2)/NativeScopeForward\A
P1::s(2)/NativeScopeForward\A#NativeScopeForward\A/NativeScopeForward\A
P1::v(2)/NativeScopeForward\A~NativeScopeForward\A/NativeScopeForward\A
T:P1::v(f2)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(f2)/NativeScopeForward\A
NativeScopeForward\A/NativeScopeForward\A NativeScopeForward\B/NativeScopeForward\B NativeScopeForward\A/NativeScopeForward\C P1::s(2)/NativeScopeForward\C#NativeScopeForward\A/NativeScopeForward\C
R:held
Q1::m/NativeScopeForward\S1:Q1::m/NativeScopeForward\S1:2
Q1::m/NativeScopeForward\S1/2
Q1::i+Q1::i
Q2::m/NativeScopeForward\S2:Q2::m/NativeScopeForward\S2:2
Q2::m/NativeScopeForward\S2/2
Q2::i+Q2::i
Q1::m/NativeScopeForward\S1:Q1::m/NativeScopeForward\S1:2
Q1::m/NativeScopeForward\S1/2
Q1::i+Q1::i
--
p1:t1 T:P1::v(0)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(0)/NativeScopeForward\A
P1::s(0)/NativeScopeForward\A#NativeScopeForward\A/NativeScopeForward\A
P1::v(0)/NativeScopeForward\A~NativeScopeForward\A/NativeScopeForward\A
T:P1::v(f0)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(f0)/NativeScopeForward\A
p2:t2 T:P2::v(0)/NativeScopeForward\B|NativeScopeForward\B/NativeScopeForward\B|P2::s(0)/NativeScopeForward\B
P2::s(0)/NativeScopeForward\B#NativeScopeForward\B/NativeScopeForward\B
P2::v(0)/NativeScopeForward\B~NativeScopeForward\B/NativeScopeForward\B
T:P2::v(f0)/NativeScopeForward\B|NativeScopeForward\B/NativeScopeForward\B|P2::s(f0)/NativeScopeForward\B
p1:t3 T:P1::v(0)/NativeScopeForward\C|NativeScopeForward\A/NativeScopeForward\C|P1::s(0)/NativeScopeForward\C
P1::s(0)/NativeScopeForward\C#NativeScopeForward\A/NativeScopeForward\C
P1::v(0)/NativeScopeForward\C~NativeScopeForward\A/NativeScopeForward\C
T:P1::v(f0)/NativeScopeForward\C|NativeScopeForward\A/NativeScopeForward\C|P1::s(f0)/NativeScopeForward\C
p1:t4 T:P1::v(0)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(0)/NativeScopeForward\A
P1::s(0)/NativeScopeForward\A#NativeScopeForward\A/NativeScopeForward\A
P1::v(0)/NativeScopeForward\A~NativeScopeForward\A/NativeScopeForward\A
T:P1::v(f0)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(f0)/NativeScopeForward\A
p2:t5 T:P2::v(0)/NativeScopeForward\B|NativeScopeForward\B/NativeScopeForward\B|P2::s(0)/NativeScopeForward\B
P2::s(0)/NativeScopeForward\B#NativeScopeForward\B/NativeScopeForward\B
P2::v(0)/NativeScopeForward\B~NativeScopeForward\B/NativeScopeForward\B
T:P2::v(f0)/NativeScopeForward\B|NativeScopeForward\B/NativeScopeForward\B|P2::s(f0)/NativeScopeForward\B
NativeScopeForward\A/NativeScopeForward\A NativeScopeForward\B/NativeScopeForward\B NativeScopeForward\A/NativeScopeForward\C P1::s(0)/NativeScopeForward\C#NativeScopeForward\A/NativeScopeForward\C
R:held
Q1::m/NativeScopeForward\S1:Q1::m/NativeScopeForward\S1:0
Q1::m/NativeScopeForward\S1/0
Q1::i+Q1::i
Q2::m/NativeScopeForward\S2:Q2::m/NativeScopeForward\S2:0
Q2::m/NativeScopeForward\S2/0
Q2::i+Q2::i
Q1::m/NativeScopeForward\S1:Q1::m/NativeScopeForward\S1:0
Q1::m/NativeScopeForward\S1/0
Q1::i+Q1::i
--
p1:t1 T:P1::v(1)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(1)/NativeScopeForward\A
P1::s(1)/NativeScopeForward\A#NativeScopeForward\A/NativeScopeForward\A
P1::v(1)/NativeScopeForward\A~NativeScopeForward\A/NativeScopeForward\A
T:P1::v(f1)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(f1)/NativeScopeForward\A
p2:t2 T:P2::v(1)/NativeScopeForward\B|NativeScopeForward\B/NativeScopeForward\B|P2::s(1)/NativeScopeForward\B
P2::s(1)/NativeScopeForward\B#NativeScopeForward\B/NativeScopeForward\B
P2::v(1)/NativeScopeForward\B~NativeScopeForward\B/NativeScopeForward\B
T:P2::v(f1)/NativeScopeForward\B|NativeScopeForward\B/NativeScopeForward\B|P2::s(f1)/NativeScopeForward\B
p1:t3 T:P1::v(1)/NativeScopeForward\C|NativeScopeForward\A/NativeScopeForward\C|P1::s(1)/NativeScopeForward\C
P1::s(1)/NativeScopeForward\C#NativeScopeForward\A/NativeScopeForward\C
P1::v(1)/NativeScopeForward\C~NativeScopeForward\A/NativeScopeForward\C
T:P1::v(f1)/NativeScopeForward\C|NativeScopeForward\A/NativeScopeForward\C|P1::s(f1)/NativeScopeForward\C
p1:t4 T:P1::v(1)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(1)/NativeScopeForward\A
P1::s(1)/NativeScopeForward\A#NativeScopeForward\A/NativeScopeForward\A
P1::v(1)/NativeScopeForward\A~NativeScopeForward\A/NativeScopeForward\A
T:P1::v(f1)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(f1)/NativeScopeForward\A
p2:t5 T:P2::v(1)/NativeScopeForward\B|NativeScopeForward\B/NativeScopeForward\B|P2::s(1)/NativeScopeForward\B
P2::s(1)/NativeScopeForward\B#NativeScopeForward\B/NativeScopeForward\B
P2::v(1)/NativeScopeForward\B~NativeScopeForward\B/NativeScopeForward\B
T:P2::v(f1)/NativeScopeForward\B|NativeScopeForward\B/NativeScopeForward\B|P2::s(f1)/NativeScopeForward\B
NativeScopeForward\A/NativeScopeForward\A NativeScopeForward\B/NativeScopeForward\B NativeScopeForward\A/NativeScopeForward\C P1::s(1)/NativeScopeForward\C#NativeScopeForward\A/NativeScopeForward\C
R:held
Q1::m/NativeScopeForward\S1:Q1::m/NativeScopeForward\S1:1
Q1::m/NativeScopeForward\S1/1
Q1::i+Q1::i
Q2::m/NativeScopeForward\S2:Q2::m/NativeScopeForward\S2:1
Q2::m/NativeScopeForward\S2/1
Q2::i+Q2::i
Q1::m/NativeScopeForward\S1:Q1::m/NativeScopeForward\S1:1
Q1::m/NativeScopeForward\S1/1
Q1::i+Q1::i
--
p1:t1 T:P1::v(2)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(2)/NativeScopeForward\A
P1::s(2)/NativeScopeForward\A#NativeScopeForward\A/NativeScopeForward\A
P1::v(2)/NativeScopeForward\A~NativeScopeForward\A/NativeScopeForward\A
T:P1::v(f2)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(f2)/NativeScopeForward\A
p2:t2 T:P2::v(2)/NativeScopeForward\B|NativeScopeForward\B/NativeScopeForward\B|P2::s(2)/NativeScopeForward\B
P2::s(2)/NativeScopeForward\B#NativeScopeForward\B/NativeScopeForward\B
P2::v(2)/NativeScopeForward\B~NativeScopeForward\B/NativeScopeForward\B
T:P2::v(f2)/NativeScopeForward\B|NativeScopeForward\B/NativeScopeForward\B|P2::s(f2)/NativeScopeForward\B
p1:t3 T:P1::v(2)/NativeScopeForward\C|NativeScopeForward\A/NativeScopeForward\C|P1::s(2)/NativeScopeForward\C
P1::s(2)/NativeScopeForward\C#NativeScopeForward\A/NativeScopeForward\C
P1::v(2)/NativeScopeForward\C~NativeScopeForward\A/NativeScopeForward\C
T:P1::v(f2)/NativeScopeForward\C|NativeScopeForward\A/NativeScopeForward\C|P1::s(f2)/NativeScopeForward\C
p1:t4 T:P1::v(2)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(2)/NativeScopeForward\A
P1::s(2)/NativeScopeForward\A#NativeScopeForward\A/NativeScopeForward\A
P1::v(2)/NativeScopeForward\A~NativeScopeForward\A/NativeScopeForward\A
T:P1::v(f2)/NativeScopeForward\A|NativeScopeForward\A/NativeScopeForward\A|P1::s(f2)/NativeScopeForward\A
p2:t5 T:P2::v(2)/NativeScopeForward\B|NativeScopeForward\B/NativeScopeForward\B|P2::s(2)/NativeScopeForward\B
P2::s(2)/NativeScopeForward\B#NativeScopeForward\B/NativeScopeForward\B
P2::v(2)/NativeScopeForward\B~NativeScopeForward\B/NativeScopeForward\B
T:P2::v(f2)/NativeScopeForward\B|NativeScopeForward\B/NativeScopeForward\B|P2::s(f2)/NativeScopeForward\B
NativeScopeForward\A/NativeScopeForward\A NativeScopeForward\B/NativeScopeForward\B NativeScopeForward\A/NativeScopeForward\C P1::s(2)/NativeScopeForward\C#NativeScopeForward\A/NativeScopeForward\C
R:held
Q1::m/NativeScopeForward\S1:Q1::m/NativeScopeForward\S1:2
Q1::m/NativeScopeForward\S1/2
Q1::i+Q1::i
Q2::m/NativeScopeForward\S2:Q2::m/NativeScopeForward\S2:2
Q2::m/NativeScopeForward\S2/2
Q2::i+Q2::i
Q1::m/NativeScopeForward\S1:Q1::m/NativeScopeForward\S1:2
Q1::m/NativeScopeForward\S1/2
Q1::i+Q1::i
--
