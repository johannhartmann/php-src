--TEST--
Array reads, isset and assignments under temporary keys
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function f(array $a, $j, $p) {
    $out = [];
    $out[] = $a[$j + 1];
    $out[] = $a[$p . "k"] ?? 'none';
    $out[] = isset($a[$j - 1]) ? 'set' : 'unset';
    $out[] = isset($a[$p . "k"]) ? 'set' : 'unset';
    $out[] = $a[strtolower("KEY")];
    $out[] = @$a[$j + 100];
    $a[$j + 2] = 'assigned';
    $a[$j * 1.0] = 'float';
    $out[] = $a;
    return $out;
}
var_dump(f([0, 1, 2, 3, 4, 'key' => 'v', 'xk' => 'XK'], 1, 'x'));
--EXPECT--
array(7) {
  [0]=>
  int(2)
  [1]=>
  string(2) "XK"
  [2]=>
  string(3) "set"
  [3]=>
  string(3) "set"
  [4]=>
  string(1) "v"
  [5]=>
  NULL
  [6]=>
  array(7) {
    [0]=>
    int(0)
    [1]=>
    string(5) "float"
    [2]=>
    int(2)
    [3]=>
    string(8) "assigned"
    [4]=>
    int(4)
    ["key"]=>
    string(1) "v"
    ["xk"]=>
    string(2) "XK"
  }
}
