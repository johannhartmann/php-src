--TEST--
Native sends of literals, undefined CVs, references and counted values
--DESCRIPTION--
Fast and universal sends store null, boolean, integer and double literals
as immediates (including the int32 boundaries and PHP_INT_MIN), copy
string and array literals, warn for an undefined CV, take a referenced CV's
value and leave a by-reference parameter to the Do; frameless calls with
literal arguments throw into a catch and out of the function.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
set_error_handler(function ($no, $msg) { echo "E: $msg\n"; return true; });
function show(...$v) { return json_encode($v); }
function two($a, $b) { return var_export($a, true) . '/' . var_export($b, true); }
function by_ref(&$r, $v) { $r = $v; return $r; }
function frameless_throws($n) { return str_repeat('ab', $n); }
function frameless_caught($n) {
    try {
        return str_repeat('ab', $n);
    } catch (ValueError $e) {
        return 'caught ' . $e->getMessage();
    }
}
function run() {
    $out = [];
    for ($i = 0; $i < 3; $i++) {
        $out[] = two(null, true);
        $out[] = two(false, 7);
        $out[] = two(PHP_INT_MAX, -PHP_INT_MAX - 1);
        $out[] = two(1.5, -0.0);
        $out[] = two(1e300, 'str');
        $out[] = two([1, 2], 0x7fffffff);
        $out[] = two(-2147483648, 2147483648);
        if ($i == 1) {
            $out[] = two($undefined, $i);
        }
        $v = $i;
        $r = &$v;
        $out[] = two($v, $r);
        $out[] = by_ref($fresh, $i) . var_export($fresh, true);
        unset($fresh, $r);
        $s = str_repeat('x', $i);
        $out[] = two($s, $s . 'y');
        $out[] = show($i, null, false, 'lit', [$i]);
        $out[] = str_replace('a', 'b', "aa$i");
        $out[] = implode(',', [1, $i]);
        $out[] = in_array($i, [0, 2], true) ? 'in' : 'out';
        $out[] = frameless_caught($i - 1);
        try {
            $out[] = frameless_throws($i - 1);
        } catch (ValueError $e) {
            $out[] = 'outer ' . $e->getMessage();
        }
    }
    return $out;
}
echo implode("\n", run()), "\n";
--EXPECT--
E: Undefined variable $undefined
NULL/true
false/7
9223372036854775807/-9223372036854775807-1
1.5/-0.0
1.0E+300/'str'
array (
  0 => 1,
  1 => 2,
)/2147483647
-2147483648/2147483648
0/0
00
''/'y'
[0,null,false,"lit",[0]]
bb0
1,0
in
caught str_repeat(): Argument #2 ($times) must be greater than or equal to 0
outer str_repeat(): Argument #2 ($times) must be greater than or equal to 0
NULL/true
false/7
9223372036854775807/-9223372036854775807-1
1.5/-0.0
1.0E+300/'str'
array (
  0 => 1,
  1 => 2,
)/2147483647
-2147483648/2147483648
NULL/1
1/1
11
'x'/'xy'
[1,null,false,"lit",[1]]
bb1
1,1
out


NULL/true
false/7
9223372036854775807/-9223372036854775807-1
1.5/-0.0
1.0E+300/'str'
array (
  0 => 1,
  1 => 2,
)/2147483647
-2147483648/2147483648
2/2
22
'xx'/'xxy'
[2,null,false,"lit",[2]]
bb2
1,2
in
ab
ab
