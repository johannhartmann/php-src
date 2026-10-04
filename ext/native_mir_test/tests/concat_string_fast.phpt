--TEST--
String concatenation without decoding the operation
--DESCRIPTION--
CONCAT and FAST_CONCAT of two strings, literals, CVs (also references) or
temporaries, into a temporary call concat_function() directly and release
temporary operands; other types and undefined CVs keep the general path.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function cat($a, $b, $n) {
    $r = &$a;
    $x = $a . $b; $y = "<$a>" . strtoupper($b); $z = $r . '' . $b; $e = '' . $b; $i = $n . $a; $f = $a . 1.5;
    return [$x, $y, $z, $e, $i, $f, str_repeat('ab', 2) . str_repeat('c', $n)];
}
for ($k = 0; $k < 3; $k++) { echo json_encode(cat("p$k", 'q', $k)), "\n"; }
echo "[" . @$undefined . "]\n";
echo "[" . $undefined2 . "]\n";
?>
--EXPECTF--
["p0q","<p0>Q","p0q","q","0p0","p01.5","abab"]
["p1q","<p1>Q","p1q","q","1p1","p11.5","ababc"]
["p2q","<p2>Q","p2q","q","2p2","p21.5","ababcc"]
[]

Warning: Undefined variable $undefined2 in %s on line 9
[]
