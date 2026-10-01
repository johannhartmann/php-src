--TEST--
Closure use() bindings copied without decoding
--DESCRIPTION--
A closure's by-value use ($x) copies the bound value into its CV without
decoding the BIND_STATIC operation, also for arrays, objects and strings;
use (&$x) and static variables keep their reference semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function counter() { static $n = 0; return ++$n; }
function run($prefix) {
    $list = [1, 2, 3]; $obj = new ArrayObject([7]); $total = 0; $str = str_repeat('s', 2);
    $map = array_map(function ($v) use ($prefix, $list, $obj, $str) { return $prefix . $v . count($list) . $obj[0] . $str; }, $list);
    $add = function ($v) use (&$total) { $total += $v; };
    array_walk($list, $add);
    $fact = function ($n) use (&$fact) { return $n <= 1 ? 1 : $n * $fact($n - 1); };
    $list[] = 4;
    $late = fn($v) => $v . count($list);
    return [implode(',', $map), $total, $fact(5), $late('x'), counter()];
}
for ($i = 0; $i < 3; $i++) { echo json_encode(run("p$i")), "\n"; }
?>
--EXPECT--
["p0137ss,p0237ss,p0337ss",6,120,"x4",1]
["p1137ss,p1237ss,p1337ss",6,120,"x4",2]
["p2137ss,p2237ss,p2337ss",6,120,"x4",3]
