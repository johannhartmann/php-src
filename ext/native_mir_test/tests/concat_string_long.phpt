--TEST--
Native concatenation of a string and an integer writes the digits once
--DESCRIPTION--
A string and a long concatenate into one new string with the long's
digits written in place, in either order, for ZEND_CONCAT and
ZEND_FAST_CONCAT, keeping the UTF-8 validity of the string side. Doubles,
booleans, null, references and arrays keep the general conversion.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function f($s, $n) { return [$s . $n, $n . $s, "k$n", "{$n}k", "pre" . $n . "post"]; }
foreach (["", "a", "key", str_repeat("x", 100), "ü", "\0"] as $s) {
    foreach ([0, 1, -1, 9, 10, -10, 123456789, PHP_INT_MAX, PHP_INT_MIN, 42] as $n) { echo json_encode(f($s, $n)), "\n"; }
}
$m = []; for ($i = 0; $i < 300; $i++) { $m["key" . $i] = $i * 3; } $sum = 0; for ($i = 0; $i < 300; $i++) { $sum += $m["key" . $i]; } var_dump($sum, count($m), array_key_first($m), array_key_last($m));
$t = "x"; $u = $t . 5; $t .= "y"; var_dump($u, $t);
$r = 7; $ref = &$r; var_dump("v" . $ref, $ref . "v");
var_dump(preg_match("/^é3$/u", "é" . 3), preg_match('/^\d+é$/u', 5 . "é"));
var_dump("a" . 1.5, "a" . true, "a" . null);
try { var_dump("a" . []); } catch (Throwable $e) { echo $e->getMessage(), "\n"; }
function g($x) { return "id-" . $x; } var_dump(g(5), g("5"), g(-0));
?>
--EXPECTF--
["0","0","k0","0k","pre0post"]
["1","1","k1","1k","pre1post"]
["-1","-1","k-1","-1k","pre-1post"]
["9","9","k9","9k","pre9post"]
["10","10","k10","10k","pre10post"]
["-10","-10","k-10","-10k","pre-10post"]
["123456789","123456789","k123456789","123456789k","pre123456789post"]
["9223372036854775807","9223372036854775807","k9223372036854775807","9223372036854775807k","pre9223372036854775807post"]
["-9223372036854775808","-9223372036854775808","k-9223372036854775808","-9223372036854775808k","pre-9223372036854775808post"]
["42","42","k42","42k","pre42post"]
["a0","0a","k0","0k","pre0post"]
["a1","1a","k1","1k","pre1post"]
["a-1","-1a","k-1","-1k","pre-1post"]
["a9","9a","k9","9k","pre9post"]
["a10","10a","k10","10k","pre10post"]
["a-10","-10a","k-10","-10k","pre-10post"]
["a123456789","123456789a","k123456789","123456789k","pre123456789post"]
["a9223372036854775807","9223372036854775807a","k9223372036854775807","9223372036854775807k","pre9223372036854775807post"]
["a-9223372036854775808","-9223372036854775808a","k-9223372036854775808","-9223372036854775808k","pre-9223372036854775808post"]
["a42","42a","k42","42k","pre42post"]
["key0","0key","k0","0k","pre0post"]
["key1","1key","k1","1k","pre1post"]
["key-1","-1key","k-1","-1k","pre-1post"]
["key9","9key","k9","9k","pre9post"]
["key10","10key","k10","10k","pre10post"]
["key-10","-10key","k-10","-10k","pre-10post"]
["key123456789","123456789key","k123456789","123456789k","pre123456789post"]
["key9223372036854775807","9223372036854775807key","k9223372036854775807","9223372036854775807k","pre9223372036854775807post"]
["key-9223372036854775808","-9223372036854775808key","k-9223372036854775808","-9223372036854775808k","pre-9223372036854775808post"]
["key42","42key","k42","42k","pre42post"]
["xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx0","0xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx","k0","0k","pre0post"]
["xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx1","1xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx","k1","1k","pre1post"]
["xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx-1","-1xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx","k-1","-1k","pre-1post"]
["xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx9","9xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx","k9","9k","pre9post"]
["xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx10","10xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx","k10","10k","pre10post"]
["xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx-10","-10xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx","k-10","-10k","pre-10post"]
["xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx123456789","123456789xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx","k123456789","123456789k","pre123456789post"]
["xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx9223372036854775807","9223372036854775807xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx","k9223372036854775807","9223372036854775807k","pre9223372036854775807post"]
["xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx-9223372036854775808","-9223372036854775808xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx","k-9223372036854775808","-9223372036854775808k","pre-9223372036854775808post"]
["xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx42","42xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx","k42","42k","pre42post"]
["\u00fc0","0\u00fc","k0","0k","pre0post"]
["\u00fc1","1\u00fc","k1","1k","pre1post"]
["\u00fc-1","-1\u00fc","k-1","-1k","pre-1post"]
["\u00fc9","9\u00fc","k9","9k","pre9post"]
["\u00fc10","10\u00fc","k10","10k","pre10post"]
["\u00fc-10","-10\u00fc","k-10","-10k","pre-10post"]
["\u00fc123456789","123456789\u00fc","k123456789","123456789k","pre123456789post"]
["\u00fc9223372036854775807","9223372036854775807\u00fc","k9223372036854775807","9223372036854775807k","pre9223372036854775807post"]
["\u00fc-9223372036854775808","-9223372036854775808\u00fc","k-9223372036854775808","-9223372036854775808k","pre-9223372036854775808post"]
["\u00fc42","42\u00fc","k42","42k","pre42post"]
["\u00000","0\u0000","k0","0k","pre0post"]
["\u00001","1\u0000","k1","1k","pre1post"]
["\u0000-1","-1\u0000","k-1","-1k","pre-1post"]
["\u00009","9\u0000","k9","9k","pre9post"]
["\u000010","10\u0000","k10","10k","pre10post"]
["\u0000-10","-10\u0000","k-10","-10k","pre-10post"]
["\u0000123456789","123456789\u0000","k123456789","123456789k","pre123456789post"]
["\u00009223372036854775807","9223372036854775807\u0000","k9223372036854775807","9223372036854775807k","pre9223372036854775807post"]
["\u0000-9223372036854775808","-9223372036854775808\u0000","k-9223372036854775808","-9223372036854775808k","pre-9223372036854775808post"]
["\u000042","42\u0000","k42","42k","pre42post"]
int(134550)
int(300)
string(4) "key0"
string(6) "key299"
string(2) "x5"
string(2) "xy"
string(2) "v7"
string(2) "7v"
int(1)
int(1)
string(4) "a1.5"
string(2) "a1"
string(1) "a"

Warning: Array to string conversion in %s on line 11
string(6) "aArray"
string(4) "id-5"
string(4) "id-5"
string(4) "id-0"
