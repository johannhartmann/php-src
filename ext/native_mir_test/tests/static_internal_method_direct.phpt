--TEST--
Native static internal method calls under the declaring class take the direct call
--DESCRIPTION--
INIT_STATIC_METHOD_CALL of a static internal method under the literal
name of its declaring class (Fiber::suspend(), Closure::bind()) calls it
directly with that class as called scope. Derived classes, static::,
self::, parent::, aliases, namespaced look-alikes and non-static methods
keep the source call for late static binding; errors and traces match.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
namespace N { class DateTime { static function createFromFormat($f, $v) { return "N\\DateTime"; } } }
namespace {
class MyDT extends DateTimeImmutable {
    static function viaStatic() { return static::createFromFormat('Y-m-d', '2024-01-02'); }
    static function viaSelf() { return self::createFromFormat('Y-m-d', '2024-01-03'); }
    static function viaParent() { return parent::createFromFormat('Y-m-d', '2024-01-04'); }
    static function viaBase() { return DateTimeImmutable::createFromFormat('Y-m-d', '2024-01-05'); }
}
class_alias('DateTimeImmutable', 'DTI');
function run() {
    $out = [];
    $out[] = get_class(DateTimeImmutable::createFromFormat('Y-m-d', '2024-02-01'));
    $out[] = get_class(datetimeimmutable::CREATEFROMFORMAT('Y-m-d', '2024-02-02'));
    $out[] = get_class(\DateTimeImmutable::createFromFormat('Y-m-d', '2024-02-03'));
    $out[] = get_class(MyDT::createFromFormat('Y-m-d', '2024-02-04'));
    $out[] = get_class(DTI::createFromFormat('Y-m-d', '2024-02-05'));
    $out[] = get_class(MyDT::viaStatic());
    $out[] = get_class(MyDT::viaSelf());
    $out[] = get_class(MyDT::viaParent());
    $out[] = get_class(MyDT::viaBase());
    $out[] = N\DateTime::createFromFormat('a', 'b');
    $out[] = var_export(DateTimeImmutable::createFromFormat('Y-m-d', 'nope'), true);
    $c = Closure::bind(function () { return get_class($this); }, new MyDT('2024-01-01'), MyDT::class);
    $out[] = $c();
    $out[] = (Closure::fromCallable('strtoupper'))('x');
    $out[] = implode(',', array_map(fn($i) => SplFixedArray::fromArray([$i, $i])->getSize(), [1, 2]));
    try { DateTimeImmutable::createFromFormat([], 'x'); } catch (TypeError $e) { $out[] = $e->getMessage(); }
    try { ArrayObject::count(); } catch (Error $e) { $out[] = $e->getMessage(); }
    try { Fiber::suspend(1); } catch (FiberError $e) { $out[] = $e->getMessage() . " @" . $e->getLine() . " " . count($e->getTrace()); }
    $f = new Fiber(function ($n) { $s = 0; for ($i = 0; $i < $n; $i++) { $s += Fiber::suspend($i * 2); } return $s; });
    $v = $f->start(4); $got = [];
    while (!$f->isTerminated()) { $got[] = $v; $v = $f->resume($v + 1); }
    $out[] = implode(',', $got) . ' => ' . $f->getReturn();
    return $out;
}
foreach (run() as $line) echo $line, "\n";
}
?>
--EXPECT--
DateTimeImmutable
DateTimeImmutable
DateTimeImmutable
MyDT
DateTimeImmutable
MyDT
MyDT
MyDT
DateTimeImmutable
N\DateTime
false
MyDT
X
2,2
DateTimeImmutable::createFromFormat(): Argument #1 ($format) must be of type string, array given
Non-static method ArrayObject::count() cannot be called statically
Cannot suspend outside of a fiber @30 2
0,2,4,6 => 16
