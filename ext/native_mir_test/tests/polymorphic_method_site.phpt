--TEST--
Native: a method site whose receivers change class takes the targets recorded per class
--DESCRIPTION--
A published method site keys one receiver class; other classes take the
target the resolver recorded for them (second level): CV, temporary and
$this receivers, overridden and inherited methods, defaults, typed
parameters, exceptions, destructors of temporary receivers, a by-reference
parameter (universal protocol) and __call.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
namespace NativePoly;

abstract class Shape {
    public function __construct(public int $n) {}
    abstract public function area(int $scale = 1): int;
    public function name() { return static::class; }
    public function twice() { return $this->area(2); }
    public function __destruct() { if ($this->n < 0) echo "destruct ", $this->n, "\n"; }
}
class Square extends Shape { public function area(int $scale = 1): int { return $this->n * $this->n * $scale; } }
class Line extends Shape { public function area(int $scale = 1): int { return 0 * $scale; } }
class Box extends Square {
    public function area(int $scale = 1): int {
        if ($this->n == 13) { throw new \RuntimeException("bad box"); }
        return parent::area($scale) * 6;
    }
}
class Ref extends Shape {
    public function area(int $scale = 1): int { return 7; }
    public function bump(&$x) { $x++; return $x; }
}
class Magic { public function __call($m, $a) { return "magic $m"; } }
class Plain { public function bump($x) { return $x + 100; } }

function total(array $shapes) {
    $sum = 0; $names = [];
    foreach ($shapes as $s) {
        $sum += $s->area();
        $sum += $s->twice();
        $names[] = $s->name();
    }
    return $sum . ' ' . implode(',', array_map(fn($n) => substr($n, 11), $names));
}

$shapes = [];
for ($i = 1; $i <= 4; $i++) {
    $shapes[] = new Square($i);
    $shapes[] = new Line($i);
    $shapes[] = new Box($i);
    $shapes[] = new Ref($i);
}
for ($round = 0; $round < 3; $round++) {
    echo total($shapes), "\n";
}
$kinds = [Square::class, Line::class, Box::class];
foreach ([1, 2, 3, 4, 5, 6] as $i) {
    $k = $kinds[$i % 3];
    echo (new $k(-$i))->area(3), "\n";
}
$x = 1;
foreach ([new Ref(1), new Plain(1), new Ref(2), new Plain(2)] as $o) {
    echo $o->bump($x), " ", $x, "\n";
}
foreach ([new Square(13), new Box(13), new Magic, new Box(2)] as $o) {
    try {
        echo $o->area(), "\n";
    } catch (\RuntimeException $e) {
        echo get_class($e), ": ", $e->getMessage(), " line ", $e->getLine(), "\n";
    }
}
?>
--EXPECT--
686 Square,Line,Box,Ref,Square,Line,Box,Ref,Square,Line,Box,Ref,Square,Line,Box,Ref
686 Square,Line,Box,Ref,Square,Line,Box,Ref,Square,Line,Box,Ref,Square,Line,Box,Ref
686 Square,Line,Box,Ref,Square,Line,Box,Ref,Square,Line,Box,Ref,Square,Line,Box,Ref
destruct -1
0
destruct -2
72
destruct -3
27
destruct -4
0
destruct -5
450
destruct -6
108
2 2
102 2
3 3
103 3
169
RuntimeException: bad box line 15
magic area
24
