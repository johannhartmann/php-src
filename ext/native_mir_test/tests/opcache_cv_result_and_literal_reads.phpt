--TEST--
Native x64 fast paths for OPcache CV results and literal array containers
--DESCRIPTION--
With OPcache the optimizer writes integer arithmetic straight into a CV and
reads dimensions from a literal array. The native fast paths must keep VM
results for integer overflow, non-integer operands, <=>, missing keys, and
non-integer elements.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
function sum_to(int $n, int $start) {
    $r = $start;
    for ($i = 0; $i < $n; $i++) {
        $r = $r + $i;
    }
    return $r;
}
function mask_sum(int $n) {
    $r = 0;
    for ($i = 0; $i < $n; $i++) {
        $k = $i & 7;
        $r += $k;
    }
    return $r;
}
function compare_all(array $values) {
    $out = [];
    foreach ($values as [$a, $b]) {
        $c = $a <=> $b;
        $out[] = $c;
    }
    return $out;
}
function packed_read(int $n) {
    $r = 0;
    for ($i = 0; $i < $n; $i++) {
        $k = $i & 7;
        $r += [1, 2, 3, 4, 5, 6, 7, 8][$k];
    }
    return $r;
}
function literal_read(int $k) {
    $v = [10, 'x', 2.5, [1]][$k];
    return $v;
}
function mixed_sum(array $values) {
    $r = 0;
    foreach ($values as $v) {
        $r = $r + $v;
    }
    return $r;
}
for ($round = 0; $round < 3; $round++) {
    var_dump(sum_to(1000, 0), sum_to(3, PHP_INT_MAX - 1), mask_sum(100));
    var_dump(compare_all([[1, 2], [2, 2], [3, 2], [PHP_INT_MIN, PHP_INT_MAX],
        [1.5, 1], ['a', 'b']]));
    var_dump(packed_read(100));
    var_dump(literal_read(0), literal_read(1), literal_read(2), literal_read(3));
    var_dump(@literal_read(9));
    var_dump(mixed_sum([1, 2.5, 3]), mixed_sum([PHP_INT_MAX, 1]));
}
?>
--EXPECT--
int(499500)
float(9.223372036854776E+18)
int(342)
array(6) {
  [0]=>
  int(-1)
  [1]=>
  int(0)
  [2]=>
  int(1)
  [3]=>
  int(-1)
  [4]=>
  int(1)
  [5]=>
  int(-1)
}
int(442)
int(10)
string(1) "x"
float(2.5)
array(1) {
  [0]=>
  int(1)
}
NULL
float(6.5)
float(9.223372036854776E+18)
int(499500)
float(9.223372036854776E+18)
int(342)
array(6) {
  [0]=>
  int(-1)
  [1]=>
  int(0)
  [2]=>
  int(1)
  [3]=>
  int(-1)
  [4]=>
  int(1)
  [5]=>
  int(-1)
}
int(442)
int(10)
string(1) "x"
float(2.5)
array(1) {
  [0]=>
  int(1)
}
NULL
float(6.5)
float(9.223372036854776E+18)
int(499500)
float(9.223372036854776E+18)
int(342)
array(6) {
  [0]=>
  int(-1)
  [1]=>
  int(0)
  [2]=>
  int(1)
  [3]=>
  int(-1)
  [4]=>
  int(1)
  [5]=>
  int(-1)
}
int(442)
int(10)
string(1) "x"
float(2.5)
array(1) {
  [0]=>
  int(1)
}
NULL
float(6.5)
float(9.223372036854776E+18)
