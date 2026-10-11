--TEST--
Native ++/--, compound assignment, long arithmetic and isset/empty at their edges
--DESCRIPTION--
++ and -- overflow exactly at PHP_INT_MAX and PHP_INT_MIN (pre and post,
through a reference and an array element), compound assignments and known
long arithmetic overflow into doubles, and isset()/empty() decide every
type; the guarded slow paths are entered from the cold area.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function incdec_bounds() {
    $out = [];
    $a = PHP_INT_MAX - 1;
    $out[] = var_export($a++, true);
    $out[] = var_export($a++, true);
    $out[] = var_export($a, true);
    $b = PHP_INT_MIN + 1;
    $out[] = var_export($b--, true);
    $out[] = var_export(--$b, true);
    $out[] = var_export($b, true);
    $c = PHP_INT_MAX;
    $out[] = var_export(++$c, true);
    $d = 5;
    $r = &$d;
    $r++;
    ++$d;
    $out[] = $d . ' ' . $r;
    $arr = [1, PHP_INT_MAX];
    $arr[0]++;
    $arr[1]++;
    $out[] = var_export($arr, true);
    $s = 0;
    for ($i = 0; $i < 5; $i++) { $s += $i; $s -= 1; $s |= 8; $s ^= 1; }
    $out[] = $s;
    $x = PHP_INT_MAX - 2;
    $x += 5;
    $out[] = var_export($x, true);
    $y = PHP_INT_MIN + 2;
    $y -= 5;
    $out[] = var_export($y, true);
    return $out;
}
function typed_math(int $a, int $b) {
    $sum = $a + $b;
    $diff = $a - $b;
    $prod = $a * $b;
    return [$sum, $diff, $prod, $a < $b, $a == $b, $a % ($b ?: 1), $a << 2, $a >> 1];
}
function isset_empty($v) {
    $r = [];
    $r[] = isset($v) ? 'set' : 'unset';
    $r[] = empty($v) ? 'empty' : 'full';
    if (isset($v)) { $r[] = 'I'; }
    if (!empty($v)) { $r[] = 'N'; }
    return implode(',', $r);
}
function release_temps($n) {
    $out = 0;
    for ($i = 0; $i < $n; $i++) {
        $tmp = str_repeat('x', $i) . 'y';
        $out += strlen($tmp);
        unset($tmp);
    }
    return $out;
}
echo implode("\n", incdec_bounds()), "\n";
foreach ([[1, 2], [PHP_INT_MAX, 1], [PHP_INT_MIN, 1], [PHP_INT_MAX, 2], [-7, 3], [0, 0]] as [$a, $b]) {
    echo json_encode(typed_math($a, $b)), "\n";
}
foreach ([null, 0, 1, '', '0', 'a', [], [0], 0.0, false, true, new stdClass] as $v) {
    echo isset_empty($v), "\n";
}
echo release_temps(10), "\n";
--EXPECT--
9223372036854775806
9223372036854775807
9.223372036854776E+18
-9223372036854775807
-9.223372036854776E+18
-9.223372036854776E+18
9.223372036854776E+18
7 7
array (
  0 => 2,
  1 => 9.223372036854776E+18,
)
12
9.223372036854776E+18
-9.223372036854776E+18
[3,-1,2,true,false,1,4,0]
[9.223372036854776e+18,9223372036854775806,9223372036854775807,false,false,0,-4,4611686018427387903]
[-9223372036854775807,-9.223372036854776e+18,-9223372036854775808,true,false,0,0,-4611686018427387904]
[9.223372036854776e+18,9223372036854775805,1.8446744073709552e+19,false,false,1,-4,4611686018427387903]
[-4,-10,-21,true,false,-1,-28,-4]
[0,0,0,false,true,0,0,0]
unset,empty
set,empty,I
set,full,I,N
set,empty,I
set,empty,I
set,full,I,N
set,empty,I
set,full,I,N
set,empty,I
set,empty,I
set,full,I,N
set,full,I,N
55
