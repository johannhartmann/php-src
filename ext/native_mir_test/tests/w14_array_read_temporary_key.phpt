--TEST--
Native x64 array reads through temporary keys
--DESCRIPTION--
x64 reads packed elements inline when the key is an integer temporary, such
as $a[$i & 7]. String, float, null and bool temporaries, missing keys,
references and non-array containers keep VM results through the helper.
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
function literal_int(int $n) {
    $r = 0;
    for ($i = 0; $i < $n; $i++) {
        $r += [1, 2, 3, 4, 5, 6, 7, 8][$i & 7];
    }
    return $r;
}
function cv_int(int $n, array $a) {
    $r = 0;
    for ($i = 0; $i < $n; $i++) {
        $r += $a[$i % 4];
    }
    return $r;
}
function string_key(array $a, string $p) {
    $out = [];
    foreach ([1, 2, 3] as $i) {
        $out[] = $a[$p . $i] ?? 'none';
        $out[] = @$a[$p . $i];
    }
    return $out;
}
function odd_keys(array $a) {
    $out = [];
    $keys = [1.7, null, true, false, -1, '2', 99];
    foreach ($keys as $k) {
        $out[] = @$a[$k === null ? null : $k];
    }
    return $out;
}
function missing_and_refs(int $n) {
    $x = 10;
    $a = [0, &$x, 'str', [5]];
    $r = [];
    for ($i = 0; $i < $n; $i++) {
        $r[] = @$a[$i + 1];
    }
    return $r;
}
function not_an_array($v) {
    return @$v[1 + 0];
}
function nested(int $n) {
    $m = [[1, 2], [3, 4]];
    $r = 0;
    for ($i = 0; $i < $n; $i++) {
        $r += $m[$i & 1][($i + 1) & 1];
    }
    return $r;
}
for ($round = 0; $round < 3; $round++) {
    var_dump(literal_int(20), cv_int(10, [1, 10, 100, 1000]));
    var_dump(string_key(['k1' => 'a', 'k3' => 'c'], 'k'));
    var_dump(odd_keys([-1 => 'm', 0 => 'z', 1 => 'o', 2 => 't', '' => 'e']));
    var_dump(missing_and_refs(4), not_an_array('xyz'), not_an_array(null), nested(5));
}
?>
--EXPECT--
int(82)
int(2233)
array(6) {
  [0]=>
  string(1) "a"
  [1]=>
  string(1) "a"
  [2]=>
  string(4) "none"
  [3]=>
  NULL
  [4]=>
  string(1) "c"
  [5]=>
  string(1) "c"
}
array(7) {
  [0]=>
  string(1) "o"
  [1]=>
  string(1) "e"
  [2]=>
  string(1) "o"
  [3]=>
  string(1) "z"
  [4]=>
  string(1) "m"
  [5]=>
  string(1) "t"
  [6]=>
  NULL
}
array(4) {
  [0]=>
  int(10)
  [1]=>
  string(3) "str"
  [2]=>
  array(1) {
    [0]=>
    int(5)
  }
  [3]=>
  NULL
}
string(1) "y"
NULL
int(12)
int(82)
int(2233)
array(6) {
  [0]=>
  string(1) "a"
  [1]=>
  string(1) "a"
  [2]=>
  string(4) "none"
  [3]=>
  NULL
  [4]=>
  string(1) "c"
  [5]=>
  string(1) "c"
}
array(7) {
  [0]=>
  string(1) "o"
  [1]=>
  string(1) "e"
  [2]=>
  string(1) "o"
  [3]=>
  string(1) "z"
  [4]=>
  string(1) "m"
  [5]=>
  string(1) "t"
  [6]=>
  NULL
}
array(4) {
  [0]=>
  int(10)
  [1]=>
  string(3) "str"
  [2]=>
  array(1) {
    [0]=>
    int(5)
  }
  [3]=>
  NULL
}
string(1) "y"
NULL
int(12)
int(82)
int(2233)
array(6) {
  [0]=>
  string(1) "a"
  [1]=>
  string(1) "a"
  [2]=>
  string(4) "none"
  [3]=>
  NULL
  [4]=>
  string(1) "c"
  [5]=>
  string(1) "c"
}
array(7) {
  [0]=>
  string(1) "o"
  [1]=>
  string(1) "e"
  [2]=>
  string(1) "o"
  [3]=>
  string(1) "z"
  [4]=>
  string(1) "m"
  [5]=>
  string(1) "t"
  [6]=>
  NULL
}
array(4) {
  [0]=>
  int(10)
  [1]=>
  string(3) "str"
  [2]=>
  array(1) {
    [0]=>
    int(5)
  }
  [3]=>
  NULL
}
string(1) "y"
NULL
int(12)
