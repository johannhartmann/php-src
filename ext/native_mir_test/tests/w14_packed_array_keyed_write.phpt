--TEST--
Native x64 in-place writes to owned packed arrays
--DESCRIPTION--
$a[$k] = $v on an owned packed array replaces an existing scalar element
inline. Copy-on-write separation, references, holes, growth, hash arrays,
counted old or new values and non-integer keys keep VM behavior.
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
function cow() { $a = [1, 2, 3]; $b = $a; for ($i = 0; $i < 3; $i++) { $a[$i] = $i * 10; } return [$a, $b]; }
function during_foreach() { $a = [1, 2, 3]; $s = []; foreach ($a as $k => $v) { $a[$k] = $v + 100; $s[] = $v; } return [$a, $s]; }
function via_ref() { $a = [1, 2]; $r = &$a; for ($i = 0; $i < 2; $i++) { $a[$i] = 7; } return $r; }
function ref_elem() { $x = 1; $a = [&$x, 2]; for ($i = 0; $i < 2; $i++) { $a[$i] = 9; } return [$a, $x]; }
function counted() { $a = ['a', 'b', 'c']; for ($i = 0; $i < 3; $i++) { $a[$i] = str_repeat('z', $i + 1); } for ($i = 0; $i < 3; $i++) { $a[$i] = $i; } return $a; }
function grow() { $a = [0]; for ($i = 0; $i < 5; $i++) { $a[$i] = $i; } return $a; }
function hash_arr() { $a = ['x' => 1, 5 => 2]; for ($i = 4; $i < 7; $i++) { $a[$i] = $i; } return $a; }
function nested() { $m = [[0, 0], [0, 0]]; for ($i = 0; $i < 2; $i++) { $row = $m[$i]; for ($j = 0; $j < 2; $j++) { $row[$j] = $i + $j; } $m[$i] = $row; } return $m; }
function neg() { $a = [1, 2]; $k = -1; $a[$k] = 5; $k = 1.5; $a[$k] = 6; return $a; }
function floats() { $a = array_fill(0, 4, 0.0); for ($i = 0; $i < 4; $i++) { $a[$i] = $i / 3; } return $a; }
function sieve_like() { $f = range(0, 20); $c = 0; for ($i = 2; $i < 21; $i++) { if ($f[$i] > 0) { for ($k = $i + $i; $k <= 20; $k += $i) { $f[$k] = 0; } $c++; } } return $c; }
function null_val() { $a = [1, 2]; $n = null; $a[0] = $n; $a[1] = true; return $a; }
for ($r = 0; $r < 2; $r++) {
    var_dump(cow(), during_foreach(), via_ref(), ref_elem(), counted(), grow(), hash_arr(), nested(), neg(), floats(), sieve_like(), null_val());
}
function by_ref_param(array &$a, $n) { for ($i = 0; $i < $n; $i++) { $a[$i] = $a[$i] + 1; } }
function typed_prop_ref() { $o = new class { public array $p = [1, 2, 3]; }; by_ref_param($o->p, 3); $r = &$o->p; $r[0] = 'x'; return $o->p; }
$arr = [5, 6, 7]; by_ref_param($arr, 3); var_dump($arr, typed_prop_ref());
$shared = [1, 2]; $copy = $shared; $ref = &$shared; by_ref_param($ref, 2); var_dump($shared, $copy);
?>
--EXPECTF--

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 10
array(2) {
  [0]=>
  array(3) {
    [0]=>
    int(0)
    [1]=>
    int(10)
    [2]=>
    int(20)
  }
  [1]=>
  array(3) {
    [0]=>
    int(1)
    [1]=>
    int(2)
    [2]=>
    int(3)
  }
}
array(2) {
  [0]=>
  array(3) {
    [0]=>
    int(101)
    [1]=>
    int(102)
    [2]=>
    int(103)
  }
  [1]=>
  array(3) {
    [0]=>
    int(1)
    [1]=>
    int(2)
    [2]=>
    int(3)
  }
}
array(2) {
  [0]=>
  int(7)
  [1]=>
  int(7)
}
array(2) {
  [0]=>
  array(2) {
    [0]=>
    int(9)
    [1]=>
    int(9)
  }
  [1]=>
  int(9)
}
array(3) {
  [0]=>
  int(0)
  [1]=>
  int(1)
  [2]=>
  int(2)
}
array(5) {
  [0]=>
  int(0)
  [1]=>
  int(1)
  [2]=>
  int(2)
  [3]=>
  int(3)
  [4]=>
  int(4)
}
array(4) {
  ["x"]=>
  int(1)
  [5]=>
  int(5)
  [4]=>
  int(4)
  [6]=>
  int(6)
}
array(2) {
  [0]=>
  array(2) {
    [0]=>
    int(0)
    [1]=>
    int(1)
  }
  [1]=>
  array(2) {
    [0]=>
    int(1)
    [1]=>
    int(2)
  }
}
array(3) {
  [0]=>
  int(1)
  [1]=>
  int(6)
  [-1]=>
  int(5)
}
array(4) {
  [0]=>
  int(0)
  [1]=>
  float(0.3333333333333333)
  [2]=>
  float(0.6666666666666666)
  [3]=>
  int(1)
}
int(8)
array(2) {
  [0]=>
  NULL
  [1]=>
  bool(true)
}

Deprecated: Implicit conversion from float 1.5 to int loses precision in %s on line 10
array(2) {
  [0]=>
  array(3) {
    [0]=>
    int(0)
    [1]=>
    int(10)
    [2]=>
    int(20)
  }
  [1]=>
  array(3) {
    [0]=>
    int(1)
    [1]=>
    int(2)
    [2]=>
    int(3)
  }
}
array(2) {
  [0]=>
  array(3) {
    [0]=>
    int(101)
    [1]=>
    int(102)
    [2]=>
    int(103)
  }
  [1]=>
  array(3) {
    [0]=>
    int(1)
    [1]=>
    int(2)
    [2]=>
    int(3)
  }
}
array(2) {
  [0]=>
  int(7)
  [1]=>
  int(7)
}
array(2) {
  [0]=>
  array(2) {
    [0]=>
    int(9)
    [1]=>
    int(9)
  }
  [1]=>
  int(9)
}
array(3) {
  [0]=>
  int(0)
  [1]=>
  int(1)
  [2]=>
  int(2)
}
array(5) {
  [0]=>
  int(0)
  [1]=>
  int(1)
  [2]=>
  int(2)
  [3]=>
  int(3)
  [4]=>
  int(4)
}
array(4) {
  ["x"]=>
  int(1)
  [5]=>
  int(5)
  [4]=>
  int(4)
  [6]=>
  int(6)
}
array(2) {
  [0]=>
  array(2) {
    [0]=>
    int(0)
    [1]=>
    int(1)
  }
  [1]=>
  array(2) {
    [0]=>
    int(1)
    [1]=>
    int(2)
  }
}
array(3) {
  [0]=>
  int(1)
  [1]=>
  int(6)
  [-1]=>
  int(5)
}
array(4) {
  [0]=>
  int(0)
  [1]=>
  float(0.3333333333333333)
  [2]=>
  float(0.6666666666666666)
  [3]=>
  int(1)
}
int(8)
array(2) {
  [0]=>
  NULL
  [1]=>
  bool(true)
}
array(3) {
  [0]=>
  int(6)
  [1]=>
  int(7)
  [2]=>
  int(8)
}
array(3) {
  [0]=>
  string(1) "x"
  [1]=>
  int(3)
  [2]=>
  int(4)
}
array(2) {
  [0]=>
  int(2)
  [1]=>
  int(3)
}
array(2) {
  [0]=>
  int(1)
  [1]=>
  int(2)
}
