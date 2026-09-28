--TEST--
Native string-key array reads and writes, branches on temporaries
--DESCRIPTION--
Array reads and writes on an array CV with a long or string key (numeric
strings name integer keys) take the element directly and release a temporary
key; missing keys, references, indirect elements and other containers keep
the general path. JMPZ/JMPNZ tests a non-counted temporary inline.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function keys() { return ["10", "-5", "05", "1.5", " 1", "9223372036854775808", "abc", "", "0", "-0", "a" . "b"]; }
function w($a) { foreach (keys() as $k) { $a[$k . ""] = strlen($k); $a[strtoupper($k)] = $k; } return $a; }
function r($a) { $o = []; foreach (keys() as $k) { $o[] = $a[$k . ""] ?? "none"; $o[] = @$a[strtolower($k) . "x"]; } return $o; }
$a = w([]); var_dump($a); var_dump(r($a));
function miss($a) { return $a[str_repeat("q", 2)]; }
var_dump(miss(["x" => 1]));
function refs() { $v = 1; $a = ["k" => &$v]; $a[str_repeat("k", 1)] = 5; $b = $a; $b[str_repeat("k", 1)] = 7; return [$v, $a["k"], $b["k"]]; }
var_dump(refs());
$gv = 3; function g() { $GLOBALS[str_repeat("g", 1) . "v"] = 9; return $GLOBALS[str_repeat("g", 1) . "v"]; } var_dump(g(), $gv);
function ao() { $o = new ArrayObject([]); $o[str_repeat("z", 2)] = 4; return $o[str_repeat("z", 2)]; } var_dump(ao());
function shared() { $a = ["x" => 1]; $b = $a; $b[str_repeat("x", 1)] = 2; return [$a, $b]; } var_dump(shared());
function h1($n) { for ($i = 1; $i <= $n; $i++) { $X[dechex($i)] = $i; } $c = 0; for ($i = $n; $i > 0; $i--) { if ($X[dechex($i)]) { $c++; } } return [$c, count($X), $X["a"], $X["10"]]; }
var_dump(h1(5000));
function t($a) { $r = []; foreach ([0, 1, "", "0", "a", [], [1], null, 0.0, 0.5, true, false] as $k => $v) { $a[$k] = $v; }
  foreach ($a as $k => $v) { if ($a[$k]) { $r[] = "T$k"; } else { $r[] = "F$k"; } if (!$a[$k]) { $r[] = "n"; } }
  if (strlen("abc")) { $r[] = "len"; } if (str_repeat("x", 2)) { $r[] = "str"; } if (str_repeat("0", 1)) { $r[] = "zero"; }
  return implode(",", $r); }
echo t([]), "\n";
function h1b($n) { for ($i = 1; $i <= $n; $i++) { $X[dechex($i)] = $i; } $c = 0; for ($i = $n; $i > 0; $i--) { if ($X[dechex($i)]) { $c++; } } return $c; }
echo h1b(3000), "\n";

?>

--EXPECTF--
array(13) {
  [10]=>
  string(2) "10"
  [-5]=>
  string(2) "-5"
  ["05"]=>
  string(2) "05"
  ["1.5"]=>
  string(3) "1.5"
  [" 1"]=>
  string(2) " 1"
  ["9223372036854775808"]=>
  string(19) "9223372036854775808"
  ["abc"]=>
  int(3)
  ["ABC"]=>
  string(3) "abc"
  [""]=>
  string(0) ""
  [0]=>
  string(1) "0"
  ["-0"]=>
  string(2) "-0"
  ["ab"]=>
  int(2)
  ["AB"]=>
  string(2) "ab"
}
array(22) {
  [0]=>
  string(2) "10"
  [1]=>
  NULL
  [2]=>
  string(2) "-5"
  [3]=>
  NULL
  [4]=>
  string(2) "05"
  [5]=>
  NULL
  [6]=>
  string(3) "1.5"
  [7]=>
  NULL
  [8]=>
  string(2) " 1"
  [9]=>
  NULL
  [10]=>
  string(19) "9223372036854775808"
  [11]=>
  NULL
  [12]=>
  int(3)
  [13]=>
  NULL
  [14]=>
  string(0) ""
  [15]=>
  NULL
  [16]=>
  string(1) "0"
  [17]=>
  NULL
  [18]=>
  string(2) "-0"
  [19]=>
  NULL
  [20]=>
  int(2)
  [21]=>
  NULL
}

Warning: Undefined array key "qq" in %s on line 6
NULL
array(3) {
  [0]=>
  int(5)
  [1]=>
  int(5)
  [2]=>
  int(7)
}
int(9)
int(9)
int(4)
array(2) {
  [0]=>
  array(1) {
    ["x"]=>
    int(1)
  }
  [1]=>
  array(1) {
    ["x"]=>
    int(2)
  }
}
array(4) {
  [0]=>
  int(5000)
  [1]=>
  int(5000)
  [2]=>
  int(10)
  [3]=>
  int(16)
}
F0,n,T1,F2,n,F3,n,T4,F5,n,T6,F7,n,F8,n,T9,T10,F11,n,len,str
3000
