--TEST--
Native appends after unsetting the last element
--DESCRIPTION--
After unset() of trailing elements the next free index lies beyond the used
packed slots; an append must turn the gap into holes rather than expose the
old elements.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
$a = ['zero', 'one', 'two'];
unset($a[2]);
$b = $a;
$a[] = 'three';
$b[] = 'three';
var_dump($a, $a === $b);
function f() {
    $a = [1, 2, 3, 4];
    unset($a[3], $a[2]);
    $a[] = 5;
    $c = [];
    unset($c[0]);
    $c[] = 1;
    $d = [0 => 'x'];
    unset($d[0]);
    $e = $d;
    $d[] = 'y';
    $e[] = 'z';
    return [$a, $c, $d, $e];
}
var_dump(f());

?>

--EXPECT--
array(3) {
  [0]=>
  string(4) "zero"
  [1]=>
  string(3) "one"
  [3]=>
  string(5) "three"
}
bool(true)
array(4) {
  [0]=>
  array(3) {
    [0]=>
    int(1)
    [1]=>
    int(2)
    [4]=>
    int(5)
  }
  [1]=>
  array(1) {
    [0]=>
    int(1)
  }
  [2]=>
  array(1) {
    [1]=>
    string(1) "y"
  }
  [3]=>
  array(1) {
    [1]=>
    string(1) "z"
  }
}
