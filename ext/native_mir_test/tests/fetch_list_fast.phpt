--TEST--
Native FETCH_LIST_R of existing elements without decoding
--DESCRIPTION--
Destructuring reads an existing array element under an integer or string
key at the helper's entry and keeps the container. Missing keys, strings,
null, references, ArrayAccess and nested patterns keep stock semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function f() { return ['a', 'b', 'k' => 'v', 5 => 'five', 'n' => ['x' => 1]]; }
function t() {
  [$a, $b] = f(); ['k' => $k, 5 => $f] = f(); list(, $second) = f(); [$c, [$d]] = [1, [2]];
  ['n' => ['x' => $x]] = f(); $arr = [3, 4]; [$p, $q] = $arr; [$m] = ['0' => 'zero'];
  [$miss1, $miss2] = [1]; $str = 'ab'; [$s0, $s1] = $str; [$z] = null;
  $ref = [&$a]; [$r] = $ref; $o = new ArrayObject([9]); [$ao] = $o;
  foreach ([[1, 2], [3, 4]] as [$u, $v]) { $sum[] = $u + $v; }
  return [$a, $b, $k, $f, $second, $c, $d, $x, $p, $q, $m, $miss1, $miss2, $s0, $s1, $z, $r, $ao, $sum];
}
echo json_encode(t()), "\n";
?>
--EXPECTF--

Warning: Undefined array key 1 in %s on line %d

Warning: Cannot use string as array in %s on line %d

Warning: Cannot use string as array in %s on line %d
["a","b","v","five","b",1,2,1,3,4,"zero",1,null,null,null,null,"a",9,[3,7]]
