--TEST--
Frameless calls from generated code: literals, references, temporaries, warnings, deprecations and exceptions
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function f($s, $a, &$r, $n) {
    $out = [];
    $out[] = strlen($s) . dechex($n + 10) . strtolower($s);
    $out[] = str_replace('a', 'b', $s) . substr($s, 1, 2) . trim("  $s ");
    $out[] = in_array($s, $a) ? 'in' : 'out';
    $out[] = str_contains($s, 'x') ? 'x' : '-';
    $r .= '!';
    $out[] = strtoupper($r) . strlen($r);
    $out[] = implode(',', $a);
    $out[] = str_replace($s, strtoupper($s), $s . $s);
    return implode('|', $out);
}
$a = ['abc', 'xyz'];
$r = 'q';
$ref = &$a[0];
$t = '';
for ($i = 0; $i < 20; $i++) {
    $t .= f($i % 2 ? 'abc' : "x$i", $a, $r, $i) . "\n";
}
echo $t;
function len($v) { return strlen($v); }
echo len('abc'), "\n";
try {
    echo len([]), "\n";
} catch (TypeError $e) {
    echo get_class($e), ': ', $e->getMessage(), "\n";
}
function rep($x) { return str_repeat('a', $x); }
try {
    rep(-1);
} catch (ValueError $e) {
    echo $e->getMessage(), "\n";
}
function tail($s) { return substr($s, 1); }
echo tail(12345), "\n";
function cut($s) { return trim($s, null); }
echo cut(" a "), "\n";
--EXPECTF--
2ax0|x00x0|out|x|Q!2|abc,xyz|X0X0
3babc|bbcbcabc|in|-|Q!!3|abc,xyz|ABCABC
2cx2|x22x2|out|x|Q!!!4|abc,xyz|X2X2
3dabc|bbcbcabc|in|-|Q!!!!5|abc,xyz|ABCABC
2ex4|x44x4|out|x|Q!!!!!6|abc,xyz|X4X4
3fabc|bbcbcabc|in|-|Q!!!!!!7|abc,xyz|ABCABC
210x6|x66x6|out|x|Q!!!!!!!8|abc,xyz|X6X6
311abc|bbcbcabc|in|-|Q!!!!!!!!9|abc,xyz|ABCABC
212x8|x88x8|out|x|Q!!!!!!!!!10|abc,xyz|X8X8
313abc|bbcbcabc|in|-|Q!!!!!!!!!!11|abc,xyz|ABCABC
314x10|x1010x10|out|x|Q!!!!!!!!!!!12|abc,xyz|X10X10
315abc|bbcbcabc|in|-|Q!!!!!!!!!!!!13|abc,xyz|ABCABC
316x12|x1212x12|out|x|Q!!!!!!!!!!!!!14|abc,xyz|X12X12
317abc|bbcbcabc|in|-|Q!!!!!!!!!!!!!!15|abc,xyz|ABCABC
318x14|x1414x14|out|x|Q!!!!!!!!!!!!!!!16|abc,xyz|X14X14
319abc|bbcbcabc|in|-|Q!!!!!!!!!!!!!!!!17|abc,xyz|ABCABC
31ax16|x1616x16|out|x|Q!!!!!!!!!!!!!!!!!18|abc,xyz|X16X16
31babc|bbcbcabc|in|-|Q!!!!!!!!!!!!!!!!!!19|abc,xyz|ABCABC
31cx18|x1818x18|out|x|Q!!!!!!!!!!!!!!!!!!!20|abc,xyz|X18X18
31dabc|bbcbcabc|in|-|Q!!!!!!!!!!!!!!!!!!!!21|abc,xyz|ABCABC
3
TypeError: strlen(): Argument #1 ($string) must be of type string, array given
str_repeat(): Argument #2 ($times) must be greater than or equal to 0
2345

Deprecated: trim(): Passing null to parameter #2 ($characters) of type string is deprecated in %s on line 37
 a 
