--TEST--
Native x64 register-key array reads through references and of scalar elements
--DESCRIPTION--
An integer-keyed packed read with a register key reads through a CV that
holds the array by reference and returns any non-refcounted scalar element
inline. Holes, strings, arrays and non-array containers keep the helper.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function sum_ref(int $n, array &$a) { $s = 0.0; for ($i = 0; $i < $n; $i++) { $s += $a[$i]; } $a[0] = $s; return $s; }
function concat_ref(int $n, &$a) { $o = ''; for ($i = 0; $i < $n; $i++) { $o .= $a[$i]; } return $o; }
function kinds(int $n, array $a) { $o = []; for ($i = 0; $i < $n; $i++) { $o[] = var_export($a[$i] ?? 'missing', true); } return implode(',', $o); }
function bad(int $n, &$a) { return $a[$n]; }
$x = [1.5, 2.5, 3, 4.25];
$y = ['a', 'b', 'c'];
$h = [0 => 1.5, 2 => 2.5];
$z = 5;
for ($k = 0; $k < 2; $k++) {
    echo sum_ref(4, $x), ' ', concat_ref(3, $y), ' ', json_encode($x), "\n";
    echo kinds(8, [1, 2.5, null, true, false, 's', [1], 0.0]), "\n";
    echo kinds(3, $h), "\n";
}
var_dump(bad(0, $z));
?>
--EXPECTF--
11.25 abc [11.25,2.5,3,4.25]
1,2.5,'missing',true,false,'s',array (
  0 => 1,
),0.0
1.5,'missing',2.5
21 abc [21,2.5,3,4.25]
1,2.5,'missing',true,false,'s',array (
  0 => 1,
),0.0
1.5,'missing',2.5

Warning: Trying to access array offset on int in %s on line %d
NULL
