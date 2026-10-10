--TEST--
Native: parameter checks of frames entered from C remember accepted classes
--DESCRIPTION--
Callbacks called from internal functions check their parameters on entry;
class-typed parameters remember the class they accepted last. Subclasses,
interfaces, unions with coercion, nullable, by-reference and wrong
arguments still take the full check.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
namespace NativeCallbackClass;

interface Node {}
class Leaf implements Node {}
class Branch extends Leaf {}
class Other { function __toString(): string { return 'other'; } }

function leaf(Leaf $l) { return get_class($l)[20]; }
function node(Node $n) { return get_class($n)[20]; }
function maybe(?Leaf $l) { return $l === null ? 'n' : 'l'; }
function either(Leaf|string $v) { return is_string($v) ? "s:$v" : 'L'; }
function scalar(int $i) { return $i * 2; }
function byRef(Leaf &$l) { $l = new Branch; return 'r'; }

$values = [new Leaf, new Branch, new Leaf, new Branch];
echo implode('', array_map('NativeCallbackClass\leaf', $values)), "\n";
echo implode('', array_map('NativeCallbackClass\node', $values)), "\n";
echo implode('', array_map('NativeCallbackClass\maybe', [null, new Leaf, null])), "\n";
echo implode(',', array_map('NativeCallbackClass\either', [new Leaf, new Other, new Branch, 'x'])), "\n";
echo implode(',', array_map('NativeCallbackClass\scalar', [1, '2', 3.0])), "\n";
$ref = new Leaf;
echo call_user_func_array('NativeCallbackClass\byRef', [&$ref]), get_class($ref), "\n";
foreach ([new Other, 'x', null] as $bad) {
    try {
        array_map('NativeCallbackClass\leaf', [new Leaf, $bad]);
    } catch (\TypeError $e) {
        echo preg_replace('/, called in .*/', '', $e->getMessage()), "\n";
    }
}
usort($values, fn(Leaf $a, Leaf $b) => strcmp(get_class($a), get_class($b)));
echo count($values), "\n";
?>
--EXPECT--
LBLB
LBLB
nln
L,s:other,L,s:x
2,4,6
rNativeCallbackClass\Branch
NativeCallbackClass\leaf(): Argument #1 ($l) must be of type NativeCallbackClass\Leaf, NativeCallbackClass\Other given
NativeCallbackClass\leaf(): Argument #1 ($l) must be of type NativeCallbackClass\Leaf, string given
NativeCallbackClass\leaf(): Argument #1 ($l) must be of type NativeCallbackClass\Leaf, null given
4
