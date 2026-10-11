--TEST--
Fused comparisons of known longs and integer literals
--DESCRIPTION--
A comparison fused into its branch compares a known long or an integer
literal without a type test, with the literal as an immediate on either
side (int32 and wider values, PHP_INT_MIN and PHP_INT_MAX bounds); doubles,
NAN, strings, null, booleans, arrays and references keep their paths.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function loops($n) {
    $out = [];
    for ($i = 0; $i < 10; $i++) { if ($i == 3) { $out[] = 'three'; } }
    $out[] = $i;
    for ($i = 0; 5 > $i; ++$i) { }
    $out[] = $i;
    for ($i = 10; 3 <= $i; --$i) { }
    $out[] = $i;
    for ($i = 0; $i <= 4000000000; $i += 1000000000) { }
    $out[] = $i;
    for ($i = -5; -2147483648 < $i; $i -= 1000000000) { }
    $out[] = $i;
    for ($i = 0; $i < $n; $i++) { }
    $out[] = $i;
    $j = 0;
    while ($j != 7) { $j++; }
    $out[] = $j;
    $k = PHP_INT_MAX - 2;
    while ($k < PHP_INT_MAX) { $k++; }
    $out[] = $k;
    $m = PHP_INT_MIN + 2;
    while (PHP_INT_MIN < $m) { $m--; }
    $out[] = $m;
    return implode(',', $out);
}
function mixed_cmp($x, $y) {
    $r = [];
    if ($x < 10) { $r[] = 'lt10'; }
    if (10 < $x) { $r[] = 'gt10'; }
    if ($x <= 2.5) { $r[] = 'le2.5'; }
    if ($x == 0) { $r[] = 'eq0'; }
    if (0 != $x) { $r[] = 'ne0'; }
    if ($x < $y) { $r[] = 'ltY'; }
    if ($x == $y) { $r[] = 'eqY'; }
    if (-1 >= $x) { $r[] = 'le-1'; }
    if ($x < 3000000000) { $r[] = 'lt3e9'; }
    if (3000000000 <= $x) { $r[] = 'ge3e9'; }
    return implode(' ', $r);
}
function refs() {
    $i = 0;
    $r = &$i;
    $c = 0;
    while ($i < 5) { $r++; $c++; }
    static $s = 0;
    while ($s < 3) { $s++; }
    return "$i $c $s";
}
echo loops(6), "\n";
foreach ([0, 1, -1, 5, 10, 11, 2.5, 2.4, NAN, INF, -INF, '0', '10', 'abc', null, true, false, 3000000000, 2999999999, [], 0.0, -0.0] as $x) {
    foreach ([0, 10, 0.5, '5'] as $y) {
        echo var_export($x, true), ' ', var_export($y, true), ': ', mixed_cmp($x, $y), "\n";
    }
}
echo refs(), "\n";
--EXPECT--
three,10,5,2,5000000000,-3000000005,6,7,9223372036854775807,-9223372036854775808
0 0: lt10 le2.5 eq0 eqY lt3e9
0 10: lt10 le2.5 eq0 ltY lt3e9
0 0.5: lt10 le2.5 eq0 ltY lt3e9
0 '5': lt10 le2.5 eq0 ltY lt3e9
1 0: lt10 le2.5 ne0 lt3e9
1 10: lt10 le2.5 ne0 ltY lt3e9
1 0.5: lt10 le2.5 ne0 lt3e9
1 '5': lt10 le2.5 ne0 ltY lt3e9
-1 0: lt10 le2.5 ne0 ltY le-1 lt3e9
-1 10: lt10 le2.5 ne0 ltY le-1 lt3e9
-1 0.5: lt10 le2.5 ne0 ltY le-1 lt3e9
-1 '5': lt10 le2.5 ne0 ltY le-1 lt3e9
5 0: lt10 ne0 lt3e9
5 10: lt10 ne0 ltY lt3e9
5 0.5: lt10 ne0 lt3e9
5 '5': lt10 ne0 eqY lt3e9
10 0: ne0 lt3e9
10 10: ne0 eqY lt3e9
10 0.5: ne0 lt3e9
10 '5': ne0 lt3e9
11 0: gt10 ne0 lt3e9
11 10: gt10 ne0 lt3e9
11 0.5: gt10 ne0 lt3e9
11 '5': gt10 ne0 lt3e9
2.5 0: lt10 le2.5 ne0 lt3e9
2.5 10: lt10 le2.5 ne0 ltY lt3e9
2.5 0.5: lt10 le2.5 ne0 lt3e9
2.5 '5': lt10 le2.5 ne0 ltY lt3e9
2.4 0: lt10 le2.5 ne0 lt3e9
2.4 10: lt10 le2.5 ne0 ltY lt3e9
2.4 0.5: lt10 le2.5 ne0 lt3e9
2.4 '5': lt10 le2.5 ne0 ltY lt3e9
NAN 0: ne0
NAN 10: ne0
NAN 0.5: ne0
NAN '5': ne0
INF 0: gt10 ne0 ge3e9
INF 10: gt10 ne0 ge3e9
INF 0.5: gt10 ne0 ge3e9
INF '5': gt10 ne0 ge3e9
-INF 0: lt10 le2.5 ne0 ltY le-1 lt3e9
-INF 10: lt10 le2.5 ne0 ltY le-1 lt3e9
-INF 0.5: lt10 le2.5 ne0 ltY le-1 lt3e9
-INF '5': lt10 le2.5 ne0 ltY le-1 lt3e9
'0' 0: lt10 le2.5 eq0 eqY lt3e9
'0' 10: lt10 le2.5 eq0 ltY lt3e9
'0' 0.5: lt10 le2.5 eq0 ltY lt3e9
'0' '5': lt10 le2.5 eq0 ltY lt3e9
'10' 0: ne0 lt3e9
'10' 10: ne0 eqY lt3e9
'10' 0.5: ne0 lt3e9
'10' '5': ne0 lt3e9
'abc' 0: gt10 ne0 ge3e9
'abc' 10: gt10 ne0 ge3e9
'abc' 0.5: gt10 ne0 ge3e9
'abc' '5': gt10 ne0 ge3e9
NULL 0: lt10 le2.5 eq0 eqY le-1 lt3e9
NULL 10: lt10 le2.5 eq0 ltY le-1 lt3e9
NULL 0.5: lt10 le2.5 eq0 ltY le-1 lt3e9
NULL '5': lt10 le2.5 eq0 ltY le-1 lt3e9
true 0: le2.5 ne0 le-1 ge3e9
true 10: le2.5 ne0 eqY le-1 ge3e9
true 0.5: le2.5 ne0 eqY le-1 ge3e9
true '5': le2.5 ne0 eqY le-1 ge3e9
false 0: lt10 le2.5 eq0 eqY le-1 lt3e9
false 10: lt10 le2.5 eq0 ltY le-1 lt3e9
false 0.5: lt10 le2.5 eq0 ltY le-1 lt3e9
false '5': lt10 le2.5 eq0 ltY le-1 lt3e9
3000000000 0: gt10 ne0 ge3e9
3000000000 10: gt10 ne0 ge3e9
3000000000 0.5: gt10 ne0 ge3e9
3000000000 '5': gt10 ne0 ge3e9
2999999999 0: gt10 ne0 lt3e9
2999999999 10: gt10 ne0 lt3e9
2999999999 0.5: gt10 ne0 lt3e9
2999999999 '5': gt10 ne0 lt3e9
array (
) 0: gt10 ne0 ge3e9
array (
) 10: gt10 ne0 ge3e9
array (
) 0.5: gt10 ne0 ge3e9
array (
) '5': gt10 ne0 ge3e9
0.0 0: lt10 le2.5 eq0 eqY lt3e9
0.0 10: lt10 le2.5 eq0 ltY lt3e9
0.0 0.5: lt10 le2.5 eq0 ltY lt3e9
0.0 '5': lt10 le2.5 eq0 ltY lt3e9
-0.0 0: lt10 le2.5 eq0 eqY lt3e9
-0.0 10: lt10 le2.5 eq0 ltY lt3e9
-0.0 0.5: lt10 le2.5 eq0 ltY lt3e9
-0.0 '5': lt10 le2.5 eq0 ltY lt3e9
5 5 3
