--TEST--
Native inline frameless calls with several literal arguments
--DESCRIPTION--
Top-level code holds the execution context, so frameless calls run
inline: str_replace(), substr(), in_array(), implode(), strpos(),
str_contains() and min() with one to three literal arguments, among
float and integer values that stay live. The literal arguments share
one load of the literal table; each argument address is built in its
argument register.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
/* Top-level code holds the execution context: frameless calls inline. */
$out = [];
for ($i = 0; $i < 50; $i++) {
    $s = ['abc', 'cab', 'bbb', 'xyz'][$i % 4];
    $v = $i % 6;
    $list = [$i, $s];
    $a = str_replace('a', 'b', $s);
    $b = substr($s, 1, 2);
    $c = in_array($v, [1, 2, 3], true) ? 'in' : 'out';
    $d = implode(',', $list);
    $e = strpos($s, 'c');
    $f = str_contains($s, 'ab');
    $g = min($v, 5);
    $h = str_replace(['a', 'c'], ['x', 'y'], $s);
    $w0 = $i + 0.5; $w1 = $i * 1.5; $w2 = $i - 2.5;
    $v0 = $i + 1; $v1 = $i * 2; $v2 = $i - 3; $v3 = $i * $i;
    $r = substr($s, 2, 3) . str_replace('b', 'q', $s) . strpos($s, 'b');
    $line = "$a|$b|$c|$d|" . var_export($e, true) . '|' . var_export($f, true)
        . "|$g|$h|$r|" . ($v0 + $v1 + $v2 + $v3) . '|' . ($w0 + $w1 + $w2);
    $out[$line] = true;
}
echo implode("\n", array_keys($out)), "\n";
--EXPECT--
bbc|bc|out|0,abc|2|true|0|xby|caqc1|-2|-2
cbb|ab|in|1,cab|0|true|1|yxb|bcaq2|3|1.5
bbb|bb|in|2,bbb|false|false|2|bbb|bqqq0|10|5
xyz|yz|in|3,xyz|false|false|3|xyz|zxyz|19|8.5
bbc|bc|out|4,abc|2|true|4|xby|caqc1|30|12
cbb|ab|out|5,cab|0|true|5|yxb|bcaq2|43|15.5
bbb|bb|out|6,bbb|false|false|0|bbb|bqqq0|58|19
xyz|yz|in|7,xyz|false|false|1|xyz|zxyz|75|22.5
bbc|bc|in|8,abc|2|true|2|xby|caqc1|94|26
cbb|ab|in|9,cab|0|true|3|yxb|bcaq2|115|29.5
bbb|bb|out|10,bbb|false|false|4|bbb|bqqq0|138|33
xyz|yz|out|11,xyz|false|false|5|xyz|zxyz|163|36.5
bbc|bc|out|12,abc|2|true|0|xby|caqc1|190|40
cbb|ab|in|13,cab|0|true|1|yxb|bcaq2|219|43.5
bbb|bb|in|14,bbb|false|false|2|bbb|bqqq0|250|47
xyz|yz|in|15,xyz|false|false|3|xyz|zxyz|283|50.5
bbc|bc|out|16,abc|2|true|4|xby|caqc1|318|54
cbb|ab|out|17,cab|0|true|5|yxb|bcaq2|355|57.5
bbb|bb|out|18,bbb|false|false|0|bbb|bqqq0|394|61
xyz|yz|in|19,xyz|false|false|1|xyz|zxyz|435|64.5
bbc|bc|in|20,abc|2|true|2|xby|caqc1|478|68
cbb|ab|in|21,cab|0|true|3|yxb|bcaq2|523|71.5
bbb|bb|out|22,bbb|false|false|4|bbb|bqqq0|570|75
xyz|yz|out|23,xyz|false|false|5|xyz|zxyz|619|78.5
bbc|bc|out|24,abc|2|true|0|xby|caqc1|670|82
cbb|ab|in|25,cab|0|true|1|yxb|bcaq2|723|85.5
bbb|bb|in|26,bbb|false|false|2|bbb|bqqq0|778|89
xyz|yz|in|27,xyz|false|false|3|xyz|zxyz|835|92.5
bbc|bc|out|28,abc|2|true|4|xby|caqc1|894|96
cbb|ab|out|29,cab|0|true|5|yxb|bcaq2|955|99.5
bbb|bb|out|30,bbb|false|false|0|bbb|bqqq0|1018|103
xyz|yz|in|31,xyz|false|false|1|xyz|zxyz|1083|106.5
bbc|bc|in|32,abc|2|true|2|xby|caqc1|1150|110
cbb|ab|in|33,cab|0|true|3|yxb|bcaq2|1219|113.5
bbb|bb|out|34,bbb|false|false|4|bbb|bqqq0|1290|117
xyz|yz|out|35,xyz|false|false|5|xyz|zxyz|1363|120.5
bbc|bc|out|36,abc|2|true|0|xby|caqc1|1438|124
cbb|ab|in|37,cab|0|true|1|yxb|bcaq2|1515|127.5
bbb|bb|in|38,bbb|false|false|2|bbb|bqqq0|1594|131
xyz|yz|in|39,xyz|false|false|3|xyz|zxyz|1675|134.5
bbc|bc|out|40,abc|2|true|4|xby|caqc1|1758|138
cbb|ab|out|41,cab|0|true|5|yxb|bcaq2|1843|141.5
bbb|bb|out|42,bbb|false|false|0|bbb|bqqq0|1930|145
xyz|yz|in|43,xyz|false|false|1|xyz|zxyz|2019|148.5
bbc|bc|in|44,abc|2|true|2|xby|caqc1|2110|152
cbb|ab|in|45,cab|0|true|3|yxb|bcaq2|2203|155.5
bbb|bb|out|46,bbb|false|false|4|bbb|bqqq0|2298|159
xyz|yz|out|47,xyz|false|false|5|xyz|zxyz|2395|162.5
bbc|bc|out|48,abc|2|true|0|xby|caqc1|2494|166
cbb|ab|in|49,cab|0|true|1|yxb|bcaq2|2595|169.5
