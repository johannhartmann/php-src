--TEST--
Native inline division, modulo, shifts and reference operands
--DESCRIPTION--
Two long operands divide, take a remainder and shift inline; an exact quotient
stays a long and an inexact one continues in double precision. Zero and -1
divisors, shift counts outside 0..63 and non-numeric operands take the helper.
CV operands bound by global, static or & are read through their reference.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function ops($a, $b) {
    $r = [];
    try { $r[] = $a / $b; } catch (Throwable $e) { $r[] = get_class($e); }
    try { $r[] = $a % $b; } catch (Throwable $e) { $r[] = get_class($e); }
    try { $r[] = $a << $b; } catch (Throwable $e) { $r[] = get_class($e); }
    try { $r[] = $a >> $b; } catch (Throwable $e) { $r[] = get_class($e); }
    return $r;
}
$vals = [0, 1, -1, 2, 3, 7, -7, 63, 64, -64, 100, PHP_INT_MAX, PHP_INT_MIN, 2.5, -0.5, "12", "x"];
foreach ($vals as $a) { foreach ($vals as $b) {
    $r = @ops($a, $b);
    echo var_export($a, true), " ", var_export($b, true), ": ", implode(" ", array_map(fn($v) => var_export($v, true), $r)), "\n";
} }
$G = 17;
function g() { global $G; return [$G / 4, $G % 5, $G << 2, $G >> 1, 100 / $G, 100 % $G, $G * 3]; }
var_dump(g());
function st() { static $s = 9; $s = $s % 4 + 10; return $s / 2; }
var_dump(st(), st());
function refs() { $x = 12; $y = &$x; $z = $y / 5; $w = $y % 5; $x = 7.5; $v = $y / 2; return [$z, $w, $v]; }
var_dump(refs());
function compound() { $a = 20; $r = &$a; $a %= 6; $a <<= 2; $a /= 3; return [$a, $r]; }
var_dump(compound());
define("IM", 139968); define("IA", 3877); define("IC", 29573);
function gen_random($n) { global $LAST; return ($n * ($LAST = ($LAST * IA + IC) % IM)) / IM; }
$LAST = 42; $s = 0; for ($i = 0; $i < 1000; $i++) { $s += gen_random(1); } var_dump($s, $LAST);

?>

--EXPECT--
0 0: 'DivisionByZeroError' 'DivisionByZeroError' 0 0
0 1: 0 0 0 0
0 -1: 0 0 'ArithmeticError' 'ArithmeticError'
0 2: 0 0 0 0
0 3: 0 0 0 0
0 7: 0 0 0 0
0 -7: 0 0 'ArithmeticError' 'ArithmeticError'
0 63: 0 0 0 0
0 64: 0 0 0 0
0 -64: 0 0 'ArithmeticError' 'ArithmeticError'
0 100: 0 0 0 0
0 9223372036854775807: 0 0 0 0
0 -9223372036854775807-1: 0 0 'ArithmeticError' 'ArithmeticError'
0 2.5: 0.0 0 0 0
0 -0.5: -0.0 'DivisionByZeroError' 0 0
0 '12': 0 0 0 0
0 'x': 'TypeError' 'TypeError' 'TypeError' 'TypeError'
1 0: 'DivisionByZeroError' 'DivisionByZeroError' 1 1
1 1: 1 0 2 0
1 -1: -1 0 'ArithmeticError' 'ArithmeticError'
1 2: 0.5 1 4 0
1 3: 0.3333333333333333 1 8 0
1 7: 0.14285714285714285 1 128 0
1 -7: -0.14285714285714285 1 'ArithmeticError' 'ArithmeticError'
1 63: 0.015873015873015872 1 -9223372036854775807-1 0
1 64: 0.015625 1 0 0
1 -64: -0.015625 1 'ArithmeticError' 'ArithmeticError'
1 100: 0.01 1 0 0
1 9223372036854775807: 1.0842021724855044E-19 1 0 0
1 -9223372036854775807-1: -1.0842021724855044E-19 1 'ArithmeticError' 'ArithmeticError'
1 2.5: 0.4 1 4 0
1 -0.5: -2.0 'DivisionByZeroError' 1 1
1 '12': 0.08333333333333333 1 4096 0
1 'x': 'TypeError' 'TypeError' 'TypeError' 'TypeError'
-1 0: 'DivisionByZeroError' 'DivisionByZeroError' -1 -1
-1 1: -1 0 -2 -1
-1 -1: 1 0 'ArithmeticError' 'ArithmeticError'
-1 2: -0.5 -1 -4 -1
-1 3: -0.3333333333333333 -1 -8 -1
-1 7: -0.14285714285714285 -1 -128 -1
-1 -7: 0.14285714285714285 -1 'ArithmeticError' 'ArithmeticError'
-1 63: -0.015873015873015872 -1 -9223372036854775807-1 -1
-1 64: -0.015625 -1 0 -1
-1 -64: 0.015625 -1 'ArithmeticError' 'ArithmeticError'
-1 100: -0.01 -1 0 -1
-1 9223372036854775807: -1.0842021724855044E-19 -1 0 -1
-1 -9223372036854775807-1: 1.0842021724855044E-19 -1 'ArithmeticError' 'ArithmeticError'
-1 2.5: -0.4 -1 -4 -1
-1 -0.5: 2.0 'DivisionByZeroError' -1 -1
-1 '12': -0.08333333333333333 -1 -4096 -1
-1 'x': 'TypeError' 'TypeError' 'TypeError' 'TypeError'
2 0: 'DivisionByZeroError' 'DivisionByZeroError' 2 2
2 1: 2 0 4 1
2 -1: -2 0 'ArithmeticError' 'ArithmeticError'
2 2: 1 0 8 0
2 3: 0.6666666666666666 2 16 0
2 7: 0.2857142857142857 2 256 0
2 -7: -0.2857142857142857 2 'ArithmeticError' 'ArithmeticError'
2 63: 0.031746031746031744 2 0 0
2 64: 0.03125 2 0 0
2 -64: -0.03125 2 'ArithmeticError' 'ArithmeticError'
2 100: 0.02 2 0 0
2 9223372036854775807: 2.168404344971009E-19 2 0 0
2 -9223372036854775807-1: -2.168404344971009E-19 2 'ArithmeticError' 'ArithmeticError'
2 2.5: 0.8 0 8 0
2 -0.5: -4.0 'DivisionByZeroError' 2 2
2 '12': 0.16666666666666666 2 8192 0
2 'x': 'TypeError' 'TypeError' 'TypeError' 'TypeError'
3 0: 'DivisionByZeroError' 'DivisionByZeroError' 3 3
3 1: 3 0 6 1
3 -1: -3 0 'ArithmeticError' 'ArithmeticError'
3 2: 1.5 1 12 0
3 3: 1 0 24 0
3 7: 0.42857142857142855 3 384 0
3 -7: -0.42857142857142855 3 'ArithmeticError' 'ArithmeticError'
3 63: 0.047619047619047616 3 -9223372036854775807-1 0
3 64: 0.046875 3 0 0
3 -64: -0.046875 3 'ArithmeticError' 'ArithmeticError'
3 100: 0.03 3 0 0
3 9223372036854775807: 3.2526065174565133E-19 3 0 0
3 -9223372036854775807-1: -3.2526065174565133E-19 3 'ArithmeticError' 'ArithmeticError'
3 2.5: 1.2 1 12 0
3 -0.5: -6.0 'DivisionByZeroError' 3 3
3 '12': 0.25 3 12288 0
3 'x': 'TypeError' 'TypeError' 'TypeError' 'TypeError'
7 0: 'DivisionByZeroError' 'DivisionByZeroError' 7 7
7 1: 7 0 14 3
7 -1: -7 0 'ArithmeticError' 'ArithmeticError'
7 2: 3.5 1 28 1
7 3: 2.3333333333333335 1 56 0
7 7: 1 0 896 0
7 -7: -1 0 'ArithmeticError' 'ArithmeticError'
7 63: 0.1111111111111111 7 -9223372036854775807-1 0
7 64: 0.109375 7 0 0
7 -64: -0.109375 7 'ArithmeticError' 'ArithmeticError'
7 100: 0.07 7 0 0
7 9223372036854775807: 7.589415207398531E-19 7 0 0
7 -9223372036854775807-1: -7.589415207398531E-19 7 'ArithmeticError' 'ArithmeticError'
7 2.5: 2.8 1 28 1
7 -0.5: -14.0 'DivisionByZeroError' 7 7
7 '12': 0.5833333333333334 7 28672 0
7 'x': 'TypeError' 'TypeError' 'TypeError' 'TypeError'
-7 0: 'DivisionByZeroError' 'DivisionByZeroError' -7 -7
-7 1: -7 0 -14 -4
-7 -1: 7 0 'ArithmeticError' 'ArithmeticError'
-7 2: -3.5 -1 -28 -2
-7 3: -2.3333333333333335 -1 -56 -1
-7 7: -1 0 -896 -1
-7 -7: 1 0 'ArithmeticError' 'ArithmeticError'
-7 63: -0.1111111111111111 -7 -9223372036854775807-1 -1
-7 64: -0.109375 -7 0 -1
-7 -64: 0.109375 -7 'ArithmeticError' 'ArithmeticError'
-7 100: -0.07 -7 0 -1
-7 9223372036854775807: -7.589415207398531E-19 -7 0 -1
-7 -9223372036854775807-1: 7.589415207398531E-19 -7 'ArithmeticError' 'ArithmeticError'
-7 2.5: -2.8 -1 -28 -2
-7 -0.5: 14.0 'DivisionByZeroError' -7 -7
-7 '12': -0.5833333333333334 -7 -28672 -1
-7 'x': 'TypeError' 'TypeError' 'TypeError' 'TypeError'
63 0: 'DivisionByZeroError' 'DivisionByZeroError' 63 63
63 1: 63 0 126 31
63 -1: -63 0 'ArithmeticError' 'ArithmeticError'
63 2: 31.5 1 252 15
63 3: 21 0 504 7
63 7: 9 0 8064 0
63 -7: -9 0 'ArithmeticError' 'ArithmeticError'
63 63: 1 0 -9223372036854775807-1 0
63 64: 0.984375 63 0 0
63 -64: -0.984375 63 'ArithmeticError' 'ArithmeticError'
63 100: 0.63 63 0 0
63 9223372036854775807: 6.830473686658678E-18 63 0 0
63 -9223372036854775807-1: -6.830473686658678E-18 63 'ArithmeticError' 'ArithmeticError'
63 2.5: 25.2 1 252 15
63 -0.5: -126.0 'DivisionByZeroError' 63 63
63 '12': 5.25 3 258048 0
63 'x': 'TypeError' 'TypeError' 'TypeError' 'TypeError'
64 0: 'DivisionByZeroError' 'DivisionByZeroError' 64 64
64 1: 64 0 128 32
64 -1: -64 0 'ArithmeticError' 'ArithmeticError'
64 2: 32 0 256 16
64 3: 21.333333333333332 1 512 8
64 7: 9.142857142857142 1 8192 0
64 -7: -9.142857142857142 1 'ArithmeticError' 'ArithmeticError'
64 63: 1.0158730158730158 1 0 0
64 64: 1 0 0 0
64 -64: -1 0 'ArithmeticError' 'ArithmeticError'
64 100: 0.64 64 0 0
64 9223372036854775807: 6.938893903907228E-18 64 0 0
64 -9223372036854775807-1: -6.938893903907228E-18 64 'ArithmeticError' 'ArithmeticError'
64 2.5: 25.6 0 256 16
64 -0.5: -128.0 'DivisionByZeroError' 64 64
64 '12': 5.333333333333333 4 262144 0
64 'x': 'TypeError' 'TypeError' 'TypeError' 'TypeError'
-64 0: 'DivisionByZeroError' 'DivisionByZeroError' -64 -64
-64 1: -64 0 -128 -32
-64 -1: 64 0 'ArithmeticError' 'ArithmeticError'
-64 2: -32 0 -256 -16
-64 3: -21.333333333333332 -1 -512 -8
-64 7: -9.142857142857142 -1 -8192 -1
-64 -7: 9.142857142857142 -1 'ArithmeticError' 'ArithmeticError'
-64 63: -1.0158730158730158 -1 0 -1
-64 64: -1 0 0 -1
-64 -64: 1 0 'ArithmeticError' 'ArithmeticError'
-64 100: -0.64 -64 0 -1
-64 9223372036854775807: -6.938893903907228E-18 -64 0 -1
-64 -9223372036854775807-1: 6.938893903907228E-18 -64 'ArithmeticError' 'ArithmeticError'
-64 2.5: -25.6 0 -256 -16
-64 -0.5: 128.0 'DivisionByZeroError' -64 -64
-64 '12': -5.333333333333333 -4 -262144 -1
-64 'x': 'TypeError' 'TypeError' 'TypeError' 'TypeError'
100 0: 'DivisionByZeroError' 'DivisionByZeroError' 100 100
100 1: 100 0 200 50
100 -1: -100 0 'ArithmeticError' 'ArithmeticError'
100 2: 50 0 400 25
100 3: 33.333333333333336 1 800 12
100 7: 14.285714285714286 2 12800 0
100 -7: -14.285714285714286 2 'ArithmeticError' 'ArithmeticError'
100 63: 1.5873015873015872 37 0 0
100 64: 1.5625 36 0 0
100 -64: -1.5625 36 'ArithmeticError' 'ArithmeticError'
100 100: 1 0 0 0
100 9223372036854775807: 1.0842021724855044E-17 100 0 0
100 -9223372036854775807-1: -1.0842021724855044E-17 100 'ArithmeticError' 'ArithmeticError'
100 2.5: 40.0 0 400 25
100 -0.5: -200.0 'DivisionByZeroError' 100 100
100 '12': 8.333333333333334 4 409600 0
100 'x': 'TypeError' 'TypeError' 'TypeError' 'TypeError'
9223372036854775807 0: 'DivisionByZeroError' 'DivisionByZeroError' 9223372036854775807 9223372036854775807
9223372036854775807 1: 9223372036854775807 0 -2 4611686018427387903
9223372036854775807 -1: -9223372036854775807 0 'ArithmeticError' 'ArithmeticError'
9223372036854775807 2: 4.611686018427388E+18 1 -4 2305843009213693951
9223372036854775807 3: 3.0744573456182584E+18 1 -8 1152921504606846975
9223372036854775807 7: 1317624576693539401 0 -128 72057594037927935
9223372036854775807 -7: -1317624576693539401 0 'ArithmeticError' 'ArithmeticError'
9223372036854775807 63: 1.464027307437266E+17 7 -9223372036854775807-1 0
9223372036854775807 64: 1.4411518807585587E+17 63 0 0
9223372036854775807 -64: -1.4411518807585587E+17 63 'ArithmeticError' 'ArithmeticError'
9223372036854775807 100: 92233720368547760.0 7 0 0
9223372036854775807 9223372036854775807: 1 0 0 0
9223372036854775807 -9223372036854775807-1: -1.0 9223372036854775807 'ArithmeticError' 'ArithmeticError'
9223372036854775807 2.5: 3.6893488147419105E+18 1 -4 2305843009213693951
9223372036854775807 -0.5: -1.8446744073709552E+19 'DivisionByZeroError' 9223372036854775807 9223372036854775807
9223372036854775807 '12': 7.686143364045646E+17 7 -4096 2251799813685247
9223372036854775807 'x': 'TypeError' 'TypeError' 'TypeError' 'TypeError'
-9223372036854775807-1 0: 'DivisionByZeroError' 'DivisionByZeroError' -9223372036854775807-1 -9223372036854775807-1
-9223372036854775807-1 1: -9223372036854775807-1 0 0 -4611686018427387904
-9223372036854775807-1 -1: 9.223372036854776E+18 0 'ArithmeticError' 'ArithmeticError'
-9223372036854775807-1 2: -4611686018427387904 0 0 -2305843009213693952
-9223372036854775807-1 3: -3.0744573456182584E+18 -2 0 -1152921504606846976
-9223372036854775807-1 7: -1.3176245766935393E+18 -1 0 -72057594037927936
-9223372036854775807-1 -7: 1.3176245766935393E+18 -1 'ArithmeticError' 'ArithmeticError'
-9223372036854775807-1 63: -1.464027307437266E+17 -8 0 -1
-9223372036854775807-1 64: -144115188075855872 0 0 -1
-9223372036854775807-1 -64: 144115188075855872 0 'ArithmeticError' 'ArithmeticError'
-9223372036854775807-1 100: -92233720368547760.0 -8 0 -1
-9223372036854775807-1 9223372036854775807: -1.0 -1 0 -1
-9223372036854775807-1 -9223372036854775807-1: 1 0 'ArithmeticError' 'ArithmeticError'
-9223372036854775807-1 2.5: -3.6893488147419105E+18 0 0 -2305843009213693952
-9223372036854775807-1 -0.5: 1.8446744073709552E+19 'DivisionByZeroError' -9223372036854775807-1 -9223372036854775807-1
-9223372036854775807-1 '12': -7.686143364045646E+17 -8 0 -2251799813685248
-9223372036854775807-1 'x': 'TypeError' 'TypeError' 'TypeError' 'TypeError'
2.5 0: 'DivisionByZeroError' 'DivisionByZeroError' 2 2
2.5 1: 2.5 0 4 1
2.5 -1: -2.5 0 'ArithmeticError' 'ArithmeticError'
2.5 2: 1.25 0 8 0
2.5 3: 0.8333333333333334 2 16 0
2.5 7: 0.35714285714285715 2 256 0
2.5 -7: -0.35714285714285715 2 'ArithmeticError' 'ArithmeticError'
2.5 63: 0.03968253968253968 2 0 0
2.5 64: 0.0390625 2 0 0
2.5 -64: -0.0390625 2 'ArithmeticError' 'ArithmeticError'
2.5 100: 0.025 2 0 0
2.5 9223372036854775807: 2.710505431213761E-19 2 0 0
2.5 -9223372036854775807-1: -2.710505431213761E-19 2 'ArithmeticError' 'ArithmeticError'
2.5 2.5: 1.0 0 8 0
2.5 -0.5: -5.0 'DivisionByZeroError' 2 2
2.5 '12': 0.20833333333333334 2 8192 0
2.5 'x': 'TypeError' 'TypeError' 'TypeError' 'TypeError'
-0.5 0: 'DivisionByZeroError' 'DivisionByZeroError' 0 0
-0.5 1: -0.5 0 0 0
-0.5 -1: 0.5 0 'ArithmeticError' 'ArithmeticError'
-0.5 2: -0.25 0 0 0
-0.5 3: -0.16666666666666666 0 0 0
-0.5 7: -0.07142857142857142 0 0 0
-0.5 -7: 0.07142857142857142 0 'ArithmeticError' 'ArithmeticError'
-0.5 63: -0.007936507936507936 0 0 0
-0.5 64: -0.0078125 0 0 0
-0.5 -64: 0.0078125 0 'ArithmeticError' 'ArithmeticError'
-0.5 100: -0.005 0 0 0
-0.5 9223372036854775807: -5.421010862427522E-20 0 0 0
-0.5 -9223372036854775807-1: 5.421010862427522E-20 0 'ArithmeticError' 'ArithmeticError'
-0.5 2.5: -0.2 0 0 0
-0.5 -0.5: 1.0 'DivisionByZeroError' 0 0
-0.5 '12': -0.041666666666666664 0 0 0
-0.5 'x': 'TypeError' 'TypeError' 'TypeError' 'TypeError'
'12' 0: 'DivisionByZeroError' 'DivisionByZeroError' 12 12
'12' 1: 12 0 24 6
'12' -1: -12 0 'ArithmeticError' 'ArithmeticError'
'12' 2: 6 0 48 3
'12' 3: 4 0 96 1
'12' 7: 1.7142857142857142 5 1536 0
'12' -7: -1.7142857142857142 5 'ArithmeticError' 'ArithmeticError'
'12' 63: 0.19047619047619047 12 0 0
'12' 64: 0.1875 12 0 0
'12' -64: -0.1875 12 'ArithmeticError' 'ArithmeticError'
'12' 100: 0.12 12 0 0
'12' 9223372036854775807: 1.3010426069826053E-18 12 0 0
'12' -9223372036854775807-1: -1.3010426069826053E-18 12 'ArithmeticError' 'ArithmeticError'
'12' 2.5: 4.8 0 48 3
'12' -0.5: -24.0 'DivisionByZeroError' 12 12
'12' '12': 1 0 49152 0
'12' 'x': 'TypeError' 'TypeError' 'TypeError' 'TypeError'
'x' 0: 'TypeError' 'TypeError' 'TypeError' 'TypeError'
'x' 1: 'TypeError' 'TypeError' 'TypeError' 'TypeError'
'x' -1: 'TypeError' 'TypeError' 'TypeError' 'TypeError'
'x' 2: 'TypeError' 'TypeError' 'TypeError' 'TypeError'
'x' 3: 'TypeError' 'TypeError' 'TypeError' 'TypeError'
'x' 7: 'TypeError' 'TypeError' 'TypeError' 'TypeError'
'x' -7: 'TypeError' 'TypeError' 'TypeError' 'TypeError'
'x' 63: 'TypeError' 'TypeError' 'TypeError' 'TypeError'
'x' 64: 'TypeError' 'TypeError' 'TypeError' 'TypeError'
'x' -64: 'TypeError' 'TypeError' 'TypeError' 'TypeError'
'x' 100: 'TypeError' 'TypeError' 'TypeError' 'TypeError'
'x' 9223372036854775807: 'TypeError' 'TypeError' 'TypeError' 'TypeError'
'x' -9223372036854775807-1: 'TypeError' 'TypeError' 'TypeError' 'TypeError'
'x' 2.5: 'TypeError' 'TypeError' 'TypeError' 'TypeError'
'x' -0.5: 'TypeError' 'TypeError' 'TypeError' 'TypeError'
'x' '12': 'TypeError' 'TypeError' 'TypeError' 'TypeError'
'x' 'x': 'TypeError' 'TypeError' 'TypeError' 'TypeError'
array(7) {
  [0]=>
  float(4.25)
  [1]=>
  int(2)
  [2]=>
  int(68)
  [3]=>
  int(8)
  [4]=>
  float(5.882352941176471)
  [5]=>
  int(15)
  [6]=>
  int(51)
}
float(5.5)
float(6.5)
array(3) {
  [0]=>
  float(2.4)
  [1]=>
  int(2)
  [2]=>
  float(3.75)
}
array(2) {
  [0]=>
  float(2.6666666666666665)
  [1]=>
  float(2.6666666666666665)
}
float(516.0704446730682)
int(11426)
