--TEST--
Native: class-typed parameters accept objects of a class they accepted before on the fast call path
--DESCRIPTION--
The native call entry compares an object argument's class with the class
the parameter last accepted; other classes, subclasses, interfaces, unions,
nullable types, Stringable coercion and type errors take the generic
receive, which remembers accepted classes. Functions with more than eight
parameters take the native call entry too.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
namespace NativeClassParam;

interface Shape { public function area(): int; }
class Box implements Shape { public function __construct(public int $n) {} public function area(): int { return $this->n * $this->n; } }
class Cube extends Box { public function area(): int { return 6 * parent::area(); } }
class Name { public function __toString(): string { return 'name'; } }

function areaOf(Shape $s): int { return $s->area(); }
function boxed(Box $b, ?Box $other = null): int { return $b->n + ($other?->n ?? 0); }
function either(Box|string $v): string { return is_string($v) ? "s:$v" : 'b:' . $v->n; }
function many(int $a, int $b, int $c, int $d, int $e, int $f, int $g, int $h, Box $i, string $j = 'j'): string {
    return ($a + $b + $c + $d + $e + $f + $g + $h + $i->n) . $j;
}

$items = [new Box(2), new Cube(1), new Box(3)];
for ($round = 0; $round < 3; $round++) {
    $out = [];
    foreach ($items as $item) {
        $out[] = areaOf($item);
        $out[] = boxed($item, $round ? $item : null);
        $out[] = either($item);
    }
    $out[] = either(new Name);
    $out[] = many(1, 2, 3, 4, 5, 6, 7, 8, $items[$round], $round ? 'x' : 'j');
    echo implode(' ', $out), "\n";
    try {
        boxed(new Name);
    } catch (\TypeError $e) {
        echo get_class($e), ': ', $e->getMessage(), "\n";
    }
}
?>
--EXPECTF--
4 2 b:2 6 1 b:1 9 3 b:3 s:name 38j
TypeError: NativeClassParam\boxed(): Argument #1 ($b) must be of type NativeClassParam\Box, NativeClassParam\Name given, called in %s on line 28
4 4 b:2 6 2 b:1 9 6 b:3 s:name 37x
TypeError: NativeClassParam\boxed(): Argument #1 ($b) must be of type NativeClassParam\Box, NativeClassParam\Name given, called in %s on line 28
4 4 b:2 6 2 b:1 9 6 b:3 s:name 39x
TypeError: NativeClassParam\boxed(): Argument #1 ($b) must be of type NativeClassParam\Box, NativeClassParam\Name given, called in %s on line 28
