--TEST--
Native: class return types accept the class a check accepted before
--DESCRIPTION--
A return-type check remembers the persistent class of the last object it
accepted; other classes, subclasses, interfaces, nullable and union types
and type errors still take the full check.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
namespace NativeReturnClass;

interface Node {}
class Leaf implements Node {}
class Branch extends Leaf {}
class Other {}

function make(int $i): Node { return $i % 3 == 0 ? new Leaf : new Branch; }
function exact(int $i): Leaf { return $i % 2 ? new Branch : new Leaf; }
function maybe(int $i): ?Leaf { return $i % 2 ? null : new Leaf; }
function either(int $i): Leaf|Other { return $i % 2 ? new Other : new Leaf; }
function wrong(int $i): Leaf { return $i < 5 ? new Leaf : new Other; }

$out = [];
for ($i = 0; $i < 7; $i++) {
    $out[] = (new \ReflectionClass(make($i)))->getShortName()[0];
    $out[] = (new \ReflectionClass(exact($i)))->getShortName()[0];
    $out[] = maybe($i) === null ? 'n' : 'l';
    $out[] = (new \ReflectionClass(either($i)))->getShortName()[0];
    try {
        $out[] = (new \ReflectionClass(wrong($i)))->getShortName()[0];
    } catch (\TypeError $e) {
        $out[] = 'E';
    }
}
echo implode('', $out), "\n";
?>
--EXPECT--
LLlLLBBnOLBLlLLLBnOLBLlLLBBnOELLlLE
