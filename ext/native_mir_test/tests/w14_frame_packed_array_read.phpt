--TEST--
Native x64 packed array reads with frame operands
--DESCRIPTION--
A dimension read whose container, key and result live in frame slots or
literals reads packed elements inline, adding a reference for counted
values. Hash arrays, string keys, holes, references and non-arrays keep
the helper.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
function reads(array $a, $k) {
    $out = [];
    for ($i = -1; $i < 5; $i++) { $out[] = @$a[$i]; }
    $out[] = @$a[$k];
    $out[] = $a[0] ?? 'd';
    return $out;
}
function strs() { $s = ['x', str_repeat('y', 3), [1, 2], new ArrayObject([])]; $r = []; for ($i = 0; $i < 4; $i++) { $v = $s[$i]; $r[] = is_object($v) ? get_class($v) : $v; } return $r; }
function refs() { $x = 1; $a = [&$x, 2]; $r = $a[0] + $a[1]; $x = 5; return [$r, $a[0]]; }
function holes() { $a = [1, 2, 3]; unset($a[1]); return [@$a[1], $a[2]]; }
function matrix(int $n) { $m = []; for ($i = 0; $i < $n; $i++) { for ($j = 0; $j < $n; $j++) { $m[$i][$j] = $i * $j; } } $s = 0; for ($i = 0; $i < $n; $i++) { $row = $m[$i]; for ($j = 0; $j < $n; $j++) { $s += $row[$j]; } } return $s; }
for ($r = 0; $r < 3; $r++) {
    var_dump(reads([10, 20.5, 'x', null], 2), reads(['a' => 1, 5 => 'five'], 'a'), reads([], 0));
    var_dump(strs(), refs(), holes(), matrix(6));
}
?>
--EXPECT--
array(8) {
  [0]=>
  NULL
  [1]=>
  int(10)
  [2]=>
  float(20.5)
  [3]=>
  string(1) "x"
  [4]=>
  NULL
  [5]=>
  NULL
  [6]=>
  string(1) "x"
  [7]=>
  int(10)
}
array(8) {
  [0]=>
  NULL
  [1]=>
  NULL
  [2]=>
  NULL
  [3]=>
  NULL
  [4]=>
  NULL
  [5]=>
  NULL
  [6]=>
  int(1)
  [7]=>
  string(1) "d"
}
array(8) {
  [0]=>
  NULL
  [1]=>
  NULL
  [2]=>
  NULL
  [3]=>
  NULL
  [4]=>
  NULL
  [5]=>
  NULL
  [6]=>
  NULL
  [7]=>
  string(1) "d"
}
array(4) {
  [0]=>
  string(1) "x"
  [1]=>
  string(3) "yyy"
  [2]=>
  array(2) {
    [0]=>
    int(1)
    [1]=>
    int(2)
  }
  [3]=>
  string(11) "ArrayObject"
}
array(2) {
  [0]=>
  int(3)
  [1]=>
  int(5)
}
array(2) {
  [0]=>
  NULL
  [1]=>
  int(3)
}
int(225)
array(8) {
  [0]=>
  NULL
  [1]=>
  int(10)
  [2]=>
  float(20.5)
  [3]=>
  string(1) "x"
  [4]=>
  NULL
  [5]=>
  NULL
  [6]=>
  string(1) "x"
  [7]=>
  int(10)
}
array(8) {
  [0]=>
  NULL
  [1]=>
  NULL
  [2]=>
  NULL
  [3]=>
  NULL
  [4]=>
  NULL
  [5]=>
  NULL
  [6]=>
  int(1)
  [7]=>
  string(1) "d"
}
array(8) {
  [0]=>
  NULL
  [1]=>
  NULL
  [2]=>
  NULL
  [3]=>
  NULL
  [4]=>
  NULL
  [5]=>
  NULL
  [6]=>
  NULL
  [7]=>
  string(1) "d"
}
array(4) {
  [0]=>
  string(1) "x"
  [1]=>
  string(3) "yyy"
  [2]=>
  array(2) {
    [0]=>
    int(1)
    [1]=>
    int(2)
  }
  [3]=>
  string(11) "ArrayObject"
}
array(2) {
  [0]=>
  int(3)
  [1]=>
  int(5)
}
array(2) {
  [0]=>
  NULL
  [1]=>
  int(3)
}
int(225)
array(8) {
  [0]=>
  NULL
  [1]=>
  int(10)
  [2]=>
  float(20.5)
  [3]=>
  string(1) "x"
  [4]=>
  NULL
  [5]=>
  NULL
  [6]=>
  string(1) "x"
  [7]=>
  int(10)
}
array(8) {
  [0]=>
  NULL
  [1]=>
  NULL
  [2]=>
  NULL
  [3]=>
  NULL
  [4]=>
  NULL
  [5]=>
  NULL
  [6]=>
  int(1)
  [7]=>
  string(1) "d"
}
array(8) {
  [0]=>
  NULL
  [1]=>
  NULL
  [2]=>
  NULL
  [3]=>
  NULL
  [4]=>
  NULL
  [5]=>
  NULL
  [6]=>
  NULL
  [7]=>
  string(1) "d"
}
array(4) {
  [0]=>
  string(1) "x"
  [1]=>
  string(3) "yyy"
  [2]=>
  array(2) {
    [0]=>
    int(1)
    [1]=>
    int(2)
  }
  [3]=>
  string(11) "ArrayObject"
}
array(2) {
  [0]=>
  int(3)
  [1]=>
  int(5)
}
array(2) {
  [0]=>
  NULL
  [1]=>
  int(3)
}
int(225)
