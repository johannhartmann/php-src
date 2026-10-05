--TEST--
Integer-key array assignments replace and append in place
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function f($n) {
    $a = [];
    for ($i = 0; $i < $n; $i++) { $a[$i] = $i * 2; }     // packed appends (growth)
    $a[$n + 5] = 'gap';                                   // beyond nNumUsed
    $b = [1, 2, 3]; unset($b[2]); $b[2] = 'back';         // append after unset of last
    $c = [1, 2, 3]; unset($c[1]); $c[1] = 'hole';         // hole: converts to hash
    $d = [1, 2]; $e = $d; $d[2] = 'sep';                  // shared: separation
    $m = [[0, 0], [0, 0]]; for ($i = 0; $i < 2; $i++) for ($j = 0; $j < 3; $j++) $m[$i][$j] = $i + $j;
    $r = [0]; $ref = &$r; $r[1] = 'viaref';
    $o = [new stdClass]; $o[1] = new stdClass; $o[0] = $o[1];
    var_dump(count($a), $a[$n - 1], $a[$n + 5], $b, $c, $d, $e, $m, $r, $o);
    $x = [5 => 'a']; $x[6] = 'b'; $x[] = 'c'; var_dump($x);
}
f(100);
--EXPECT--
int(101)
int(198)
string(3) "gap"
array(3) {
  [0]=>
  int(1)
  [1]=>
  int(2)
  [2]=>
  string(4) "back"
}
array(3) {
  [0]=>
  int(1)
  [2]=>
  int(3)
  [1]=>
  string(4) "hole"
}
array(3) {
  [0]=>
  int(1)
  [1]=>
  int(2)
  [2]=>
  string(3) "sep"
}
array(2) {
  [0]=>
  int(1)
  [1]=>
  int(2)
}
array(2) {
  [0]=>
  array(3) {
    [0]=>
    int(0)
    [1]=>
    int(1)
    [2]=>
    int(2)
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
  int(0)
  [1]=>
  string(6) "viaref"
}
array(2) {
  [0]=>
  object(stdClass)#2 (0) {
  }
  [1]=>
  object(stdClass)#2 (0) {
  }
}
array(3) {
  [5]=>
  string(1) "a"
  [6]=>
  string(1) "b"
  [7]=>
  string(1) "c"
}
