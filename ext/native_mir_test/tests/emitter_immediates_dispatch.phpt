--TEST--
Native switch, match, literal arrays, dim compound assignment and return types with immediates
--DESCRIPTION--
SWITCH_STRING compares each case's length and bytes with immediates
(widths 8, 4, 2 and 1, high-bit bytes), SWITCH_LONG and dense integer
switches dispatch with immediates, literal-keyed arrays share one literal
table load, $a[literal] op= literal takes only its key's arm, a return
type mask is an immediate and a numeric entry variant takes doubles out of
line.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function sw_str($s) {
    switch ($s) {
        case 'a': return 1;
        case 'ab': return 2;
        case 'abc': return 3;
        case 'abcd': return 4;
        case 'abcdefgh': return 8;
        case 'abcdefghi': return 9;
        case 'abcdefghijklmnopq': return 17;
        case "\xff\xfe\x80\x81": return 44;
        case "\xff\xff\xff\xff\xff\xff\xff\x7f": return 88;
        case "\x80\x00\x00\x00\x00\x00\x00\x80": return 89;
        default: return 0;
    }
}
function sw_int($i) {
    switch ($i) {
        case -5: return 'm5';
        case 0: return 'z';
        case 1: return 'one';
        case 2: return 'two';
        case 3: return 'three';
        case 7: return 'seven';
        case 3000000000: return 'big';
        case PHP_INT_MIN: return 'min';
        default: return 'd';
    }
}
function sw_dense($i) {
    switch ($i) {
        case 10: return 'a'; case 11: return 'b'; case 12: return 'c';
        case 13: return 'd'; case 14: return 'e'; case 15: return 'f';
        case 16: return 'g'; case 17: return 'h'; default: return '-';
    }
}
function m($v) {
    return match ($v) {
        1, 2 => 'low',
        'x', 'yy' => 'str',
        100 => 'hundred',
        default => 'other',
    };
}
function arr($v) {
    $a = ['name' => 'n', 'v' => $v, 'k' => 1.5, 'z' => null];
    $b = ['x', 'y', $v];
    return json_encode($a) . json_encode($b);
}
function dim_op() {
    $p = [1, 2, 3];
    $p[1] += 5;
    $p[0] -= 1.5;
    $p[2] *= 3;
    $h = ['a' => 1, 'b' => 2.5, 'c' => PHP_INT_MAX];
    $h['a'] += 10;
    $h['b'] *= 2;
    $h['c'] += 1;
    $h['new'] = 0;
    $h['new'] += 3;
    $k = 'a';
    $h[$k] -= 2;
    try {
        $p[7] += 1;
    } catch (Throwable $e) {
    }
    $s = ['s' => '5'];
    $s['s'] += 1;
    return json_encode([$p, $h, $s]);
}
function ret_int($v): int { return $v; }
function ret_union($v): int|string { return $v; }
function ret_nullable($v): ?array { return $v; }
function num_variant($a, $b) { return $a * 2 + $b; }
set_error_handler(function ($no, $msg) { echo "E: $msg\n"; return true; });
foreach (['a', 'ab', 'abc', 'abcd', 'abcdefgh', 'abcdefghi', 'abcdefghijklmnopq', "\xff\xfe\x80\x81",
         "\xff\xff\xff\xff\xff\xff\xff\x7f", "\x80\x00\x00\x00\x00\x00\x00\x80", 'abce', 'b', '', 'abcdefghijklmnopr'] as $s) {
    echo bin2hex($s), ' ', sw_str($s), "\n";
}
foreach ([-5, 0, 1, 2, 3, 4, 7, 3000000000, PHP_INT_MIN, '1', 1.0, null] as $i) {
    echo var_export($i, true), ' ', sw_int($i), ' ', sw_dense(is_int($i) ? $i + 12 : $i), "\n";
}
foreach ([1, 2, 'x', 'yy', 100, 3, '1', 'z'] as $v) { echo m($v), "\n"; }
echo arr(3), "\n", arr('q'), "\n";
echo dim_op(), "\n";
foreach ([1, '2', 3.0, 'x', null, true] as $v) {
    try { echo var_export(ret_int($v), true), "\n"; } catch (TypeError $e) { echo "TE int\n"; }
    try { echo var_export(ret_union($v), true), "\n"; } catch (TypeError $e) { echo "TE union\n"; }
    try { echo var_export(ret_nullable($v), true), "\n"; } catch (TypeError $e) { echo "TE nullable\n"; }
}
for ($i = 0; $i < 3; $i++) {
    echo num_variant($i, 1), ' ', num_variant($i + 0.5, 1), ' ', num_variant('3', $i), "\n";
}
--EXPECT--
61 1
6162 2
616263 3
61626364 4
6162636465666768 8
616263646566676869 9
6162636465666768696a6b6c6d6e6f7071 17
fffe8081 44
ffffffffffffff7f 88
8000000000000080 89
61626365 0
62 0
 0
6162636465666768696a6b6c6d6e6f7072 0
-5 m5 -
0 z c
1 one d
2 two e
3 three f
4 d g
7 seven -
3000000000 big -
-9223372036854775807-1 min -
'1' one -
1.0 one -
NULL z -
low
low
str
str
hundred
other
other
other
{"name":"n","v":3,"k":1.5,"z":null}["x","y",3]
{"name":"n","v":"q","k":1.5,"z":null}["x","y","q"]
E: Undefined array key 7
[{"0":-0.5,"1":7,"2":9,"7":1},{"a":9,"b":5,"c":9.223372036854776e+18,"new":3},{"s":6}]
1
1
TE nullable
2
'2'
TE nullable
3
3
TE nullable
TE int
'x'
TE nullable
TE int
TE union
NULL
1
1
TE nullable
1 2 6
3 4 7
5 6 8
