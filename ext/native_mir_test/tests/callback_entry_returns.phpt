--TEST--
Native: frames entered from C return and receive as the VM does
--DESCRIPTION--
Callbacks entered from internal functions: typed and untyped returns
checked by the type mask or the full check, coercion, void, never,
by-reference returns, extra and missing arguments.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
namespace NativeCallbackReturns;

class Leaf {}
function ints(array $a) { return array_map(fn(int $x): int => $x * 2, $a); }
function strs(array $a) { return array_map(fn($x): string => $x, $a); }
function nullable(array $a) { return array_map(fn($x): ?Leaf => $x ? new Leaf : null, $a); }
function voids(array $a) { $n = 0; array_walk($a, function ($x) use (&$n): void { $n += $x; }); return $n; }
function nevers() { return array_map(function ($x): never { throw new \LogicException("never $x"); }, [1]); }
function bad() { return array_map(fn($x): int => $x, ['x']); }
function coerce() { return array_map(fn($x): int => $x, ['5', 6.0]); }
function byref() { $v = [1]; return array_map(function &($x) use (&$v) { return $v[0]; }, [1, 2]); }
function extra() { return array_map(fn($x) => func_get_args(), [1]); }
function missing() { return array_map(fn($x, $y) => $y, [1]); }

for ($i = 0; $i < 3; $i++) {
    echo json_encode([ints([1, 2]), strs(['a']), count(array_filter(nullable([0, 1]))), voids([1, 2, 3]), coerce(), byref(), extra()]), "\n";
}
foreach (['nevers', 'bad', 'missing'] as $f) {
    try {
        ('NativeCallbackReturns\\' . $f)();
    } catch (\Throwable $e) {
        echo get_class($e), ': ', preg_replace('/, called in .*/', '', $e->getMessage()), "\n";
    }
}
echo strs([1])[0] === '1' ? "coerced\n" : "not\n";
?>
--EXPECT--
[[2,4],["a"],1,6,[5,6],[1,1],[[1]]]
[[2,4],["a"],1,6,[5,6],[1,1],[[1]]]
[[2,4],["a"],1,6,[5,6],[1,1],[[1]]]
LogicException: never 1
TypeError: {closure:NativeCallbackReturns\bad():10}(): Return value must be of type int, string returned
ArgumentCountError: Too few arguments to function {closure:NativeCallbackReturns\missing():14}(), 1 passed and exactly 2 expected
coerced
